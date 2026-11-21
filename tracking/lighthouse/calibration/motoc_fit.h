// SPDX-License-Identifier: GPL-3.0-only
// Adapted from galister/motoc libmotoc/src/calibrator/sampled.rs.
// Original method implements pushrax/OpenVR-SpaceCalibrator mathematics.
// See COPYING and README.md in this directory.
#pragma once
#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <vector>

namespace vrcm_calibration {
using Pose = Eigen::Isometry3d;
struct Sample { Pose quest, lighthouse; };
class ResidualError : public std::runtime_error {
public:
    double score;
    ResidualError(const std::string& message,double quality) : std::runtime_error(message),score(quality) {}
};
struct Fit {
    Pose questFromLighthouse = Pose::Identity();
    Pose trackerFromHead = Pose::Identity();
    double positionRms = 0, angleRms = 0;
    size_t rotationPairs = 0;
};
inline void checkFitQuality(const Fit& fit) {
    if (!(fit.positionRms < 0.03 && fit.angleRms < 0.05)) {
        std::ostringstream message;
        message<<"Head poses disagree: "<<std::fixed<<std::setprecision(1)<<fit.positionRms*1000
               <<" mm and "<<fit.angleRms*180/3.141592653589793<<" degrees; check tracking stability and timing";
        throw ResidualError(message.str(),fit.positionRms+.1*fit.angleRms);
    }
}

inline void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
inline bool valid(const Pose &p) {
    return p.matrix().allFinite() &&
        (p.rotation().transpose() * p.rotation() - Eigen::Matrix3d::Identity()).norm() < 1e-5 &&
        std::abs(p.rotation().determinant() - 1) < 1e-5;
}
inline Eigen::Vector3d axis(const Eigen::Matrix3d &m) {
    return {m(2,1)-m(1,2), m(0,2)-m(2,0), m(1,0)-m(0,1)};
}
inline double angle(const Eigen::Matrix3d &m) {
    return std::acos(std::clamp((m.trace()-1)*0.5, -1.0, 1.0));
}

// Samples must be from matching timestamps, in raw stable tracking origins,
// with the tracker rigidly held on the headset for the whole capture.
inline Fit solve(const std::vector<Sample> &samples, bool enforceQuality=true) {
    require(samples.size() >= 30 && samples.size() <= 600, "Need 30–600 synchronized samples");
    for (const auto &s : samples)
        require(valid(s.quest) && valid(s.lighthouse), "Invalid or untracked input pose");
    std::vector<std::pair<Eigen::Vector3d, Eigen::Vector3d>> deltas;
    Eigen::Vector3d am = Eigen::Vector3d::Zero(), bm = am;
    for (size_t i=0; i<samples.size(); ++i) for (size_t j=0; j<i; ++j) {
        Eigen::Matrix3d a = samples[i].quest.rotation()*samples[j].quest.rotation().transpose();
        Eigen::Matrix3d b = samples[i].lighthouse.rotation()*samples[j].lighthouse.rotation().transpose();
        Eigen::Vector3d av=axis(a), bv=axis(b);
        if (angle(a)<0.4 || angle(b)<0.4 || av.squaredNorm()<0.1 || bv.squaredNorm()<0.1) continue;
        av.normalize(); bv.normalize();
        deltas.emplace_back(av,bv); am+=av; bm+=bv;
    }
    require(deltas.size() >= 20, "Not enough head rotation: turn left/right and nod by about 30 degrees");
    am/=double(deltas.size()); bm/=double(deltas.size());
    Eigen::Matrix3d cross = Eigen::Matrix3d::Zero();
    for (const auto &d : deltas) cross+=(d.first-am)*(d.second-bm).transpose();
    Eigen::JacobiSVD<Eigen::Matrix3d> svd(cross,Eigen::ComputeFullU|Eigen::ComputeFullV);
    require(svd.singularValues()[1] > svd.singularValues()[0]*1e-3,
            "Rotation samples are degenerate; use multiple axes");
    Eigen::Matrix3d sign=Eigen::Matrix3d::Identity();
    if ((svd.matrixU()*svd.matrixV().transpose()).determinant()<0) sign(2,2)=-1;
    Eigen::Matrix3d rotation=svd.matrixU()*sign*svd.matrixV().transpose();

    // Equivalent to Motoc's stacked least-squares translation solve. Accumulate
    // the 3x3 normal system to avoid a large allocation for all sample pairs.
    Eigen::Matrix3d lhs=Eigen::Matrix3d::Zero();
    Eigen::Vector3d rhs=Eigen::Vector3d::Zero();
    for (size_t i=0; i<samples.size(); ++i) for (size_t j=0; j<i; ++j) {
        const auto &si=samples[i]; const auto &sj=samples[j];
        Eigen::Vector3d di=si.quest.translation()-rotation*si.lighthouse.translation();
        Eigen::Vector3d dj=sj.quest.translation()-rotation*sj.lighthouse.translation();
        for (int kind=0; kind<2; ++kind) {
            Eigen::Matrix3d ri=kind ? (rotation*si.lighthouse.rotation()).transpose().eval() : si.quest.rotation().transpose().eval();
            Eigen::Matrix3d rj=kind ? (rotation*sj.lighthouse.rotation()).transpose().eval() : sj.quest.rotation().transpose().eval();
            Eigen::Matrix3d a=rj-ri;
            Eigen::Vector3d b=rj*dj-ri*di;
            lhs+=a.transpose()*a; rhs+=a.transpose()*b;
        }
    }
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> rank(lhs);
    require(rank.eigenvalues()[0] > rank.eigenvalues()[2]*1e-6,
            "Translation is underconstrained; use more varied rotations");
    Fit fit;
    fit.questFromLighthouse.linear()=rotation;
    fit.questFromLighthouse.translation()=lhs.ldlt().solve(rhs);
    fit.rotationPairs=deltas.size();
    require(valid(fit.questFromLighthouse), "Calibration produced an invalid transform");
    require(fit.questFromLighthouse.translation().norm()<100, "Implausible tracking-space separation");

    // Estimate the fixed headset-to-tracker mounting transform. It must not
    // be mistaken for the transform between the two tracking origins.
    Eigen::Matrix4d mean=Eigen::Matrix4d::Zero();
    Eigen::Vector3d position=Eigen::Vector3d::Zero();
    for (const auto &s : samples) {
        Pose mount=(fit.questFromLighthouse*s.lighthouse).inverse()*s.quest;
        position+=mount.translation();
        Eigen::Vector4d q=Eigen::Quaterniond(mount.rotation()).coeffs();
        mean+=q*q.transpose();
    }
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix4d> qs(mean);
    Eigen::Quaterniond q; q.coeffs()=qs.eigenvectors().col(3);
    fit.trackerFromHead.linear()=q.normalized().toRotationMatrix();
    fit.trackerFromHead.translation()=position/double(samples.size());
    for (const auto &s : samples) {
        Pose predicted=fit.questFromLighthouse*s.lighthouse*fit.trackerFromHead;
        fit.positionRms+=(predicted.translation()-s.quest.translation()).squaredNorm();
        double error=angle(predicted.rotation().transpose()*s.quest.rotation());
        fit.angleRms+=error*error;
    }
    fit.positionRms=std::sqrt(fit.positionRms/samples.size());
    fit.angleRms=std::sqrt(fit.angleRms/samples.size());
    if(enforceQuality) checkFitQuality(fit);
    return fit;
}

struct RobustFit { Fit fit; size_t rejectedSamples=0; };
// Least-trimmed calibration: tolerate a small minority of pose corrections,
// retaining at least 90% of pairs and the caller's minimum sample count.
// Every accepted result still passes the same position/angular RMS gate.
inline RobustFit solveRobust(const std::vector<Sample>& samples,size_t minimumSamples=30) {
    require(samples.size()>=minimumSamples,"Not enough paired samples");
    Fit current=solve(samples,false);
    try {checkFitQuality(current);return {current,0};} catch(const ResidualError&) {}
    size_t keep=std::max(minimumSamples,samples.size()-samples.size()/10);
    if(keep>=samples.size()) {checkFitQuality(current);return {current,0};}
    Fit best=current;double bestScore=current.positionRms+.1*current.angleRms;
    for(int iteration=0;iteration<4;++iteration) {
        std::vector<std::pair<double,size_t>> scores;
        for(size_t i=0;i<samples.size();++i) {
            Pose predicted=current.questFromLighthouse*samples[i].lighthouse*current.trackerFromHead;
            double position=(predicted.translation()-samples[i].quest.translation()).norm()/.03;
            double rotation=angle(predicted.rotation().transpose()*samples[i].quest.rotation())/.05;
            scores.push_back({position*position+rotation*rotation,i});
        }
        std::sort(scores.begin(),scores.end());std::vector<size_t> indices;
        for(size_t i=0;i<keep;++i) indices.push_back(scores[i].second);
        std::sort(indices.begin(),indices.end());std::vector<Sample> inliers;
        for(auto i:indices) inliers.push_back(samples[i]);
        current=solve(inliers,false);
        double score=current.positionRms+.1*current.angleRms;
        if(score<bestScore) {best=current;bestScore=score;}
    }
    checkFitQuality(best);
    return {best,samples.size()-keep};
}

// Continuous mode's correction equation, from Motoc's offset.rs. The caller
// owns the current profile and supplies raw, time-aligned tracker poses.
// Reject fast motion or lost tracking rather than fitting prediction errors.
inline bool maintain(Pose &questFromLighthouse, const Pose &trackerFromHead,
                     const Sample &sample, bool bothTracked, int64_t timestampSkewNs,
                     double maxLinearSpeed, double maxAngularSpeed, double elapsedSeconds) {
    if (!bothTracked || timestampSkewNs < -5000000 || timestampSkewNs > 5000000 ||
        !std::isfinite(maxLinearSpeed) || !std::isfinite(maxAngularSpeed) ||
        maxLinearSpeed < 0 || maxAngularSpeed < 0 ||
        maxLinearSpeed > 0.5 || maxAngularSpeed > 0.3 ||
        !std::isfinite(elapsedSeconds) || elapsedSeconds <= 0 || elapsedSeconds > 0.1 ||
        !valid(questFromLighthouse) || !valid(trackerFromHead) ||
        !valid(sample.quest) || !valid(sample.lighthouse)) return false;
    Pose desired=sample.quest*trackerFromHead.inverse()*sample.lighthouse.inverse();
    // A jump is evidence of a changed stage or bad tracking. Require explicit
    // recalibration rather than silently pulling the user's body across space.
    if ((desired.translation()-questFromLighthouse.translation()).norm()>0.15 ||
        angle(desired.rotation()*questFromLighthouse.rotation().transpose())>0.2) return false;
    // Time-based equivalent of a small per-frame interpolation: no dependence
    // on whether the headset runs at 72, 80, 90 or 120 Hz.
    double alpha=1-std::exp(-elapsedSeconds/1.0);
    Eigen::Quaterniond from(questFromLighthouse.rotation()), to(desired.rotation());
    questFromLighthouse.linear()=from.slerp(alpha,to).normalized().toRotationMatrix();
    questFromLighthouse.translation()=(1-alpha)*questFromLighthouse.translation()+alpha*desired.translation();
    return true;
}
} // namespace vrcm_calibration
