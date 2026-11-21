#define main saved_replay_main
#include "replay_fit.cpp"
#undef main
static Eigen::Matrix3d fitAxes(const std::vector<Sample>& s,int mode) {
    Eigen::Matrix3d cross=Eigen::Matrix3d::Zero();
    for(size_t i=0;i<s.size();++i) for(size_t j=0;j<i;++j) {
        Eigen::Matrix3d a=s[i].quest.rotation()*s[j].quest.rotation().transpose();
        Eigen::Matrix3d b=s[i].lighthouse.rotation()*s[j].lighthouse.rotation().transpose();
        Eigen::AngleAxisd aa(a),bb(b);if(aa.angle()<.1 || bb.angle()<.1) continue;
        Eigen::Vector3d av=aa.axis(),bv=bb.axis();
        if(mode) {av*=aa.angle();bv*=bb.angle();}
        cross+=av*bv.transpose();
    }
    Eigen::JacobiSVD<Eigen::Matrix3d> svd(cross,Eigen::ComputeFullU|Eigen::ComputeFullV);
    Eigen::Matrix3d sign=Eigen::Matrix3d::Identity();if((svd.matrixU()*svd.matrixV().transpose()).determinant()<0) sign(2,2)=-1;
    return svd.matrixU()*sign*svd.matrixV().transpose();
}
static void evaluate(const std::vector<Sample>& samples,const Eigen::Matrix3d& r,const char* name) {
    Eigen::Matrix4d qsum=Eigen::Matrix4d::Zero();
    Eigen::Matrix<double,6,6> lhs=Eigen::Matrix<double,6,6>::Zero();Eigen::Matrix<double,6,1> rhs=Eigen::Matrix<double,6,1>::Zero();
    for(auto& s:samples) {
        Eigen::Vector4d q=Eigen::Quaterniond((r*s.lighthouse.rotation()).transpose()*s.quest.rotation()).coeffs();qsum+=q*q.transpose();
        Eigen::Matrix<double,3,6> a;a.leftCols<3>()=Eigen::Matrix3d::Identity();a.rightCols<3>()=r*s.lighthouse.rotation();
        Eigen::Vector3d b=s.quest.translation()-r*s.lighthouse.translation();lhs+=a.transpose()*a;rhs+=a.transpose()*b;
    }
    Eigen::Quaterniond mount;mount.coeffs()=Eigen::SelfAdjointEigenSolver<Eigen::Matrix4d>(qsum).eigenvectors().col(3);
    Eigen::Matrix<double,6,1> x=lhs.ldlt().solve(rhs);
    double angleError=0,posError=0;
    for(auto& s:samples) {
        double error=angle((r*s.lighthouse.rotation()*mount.toRotationMatrix()).transpose()*s.quest.rotation());angleError+=error*error;
        posError+=(r*s.lighthouse.translation()+x.head<3>()+r*s.lighthouse.rotation()*x.tail<3>()-s.quest.translation()).squaredNorm();
    }
    std::cout<<name<<": "<<sqrt(posError/samples.size())*1000<<" mm, "<<sqrt(angleError/samples.size())*180/M_PI<<" deg; mount "<<x.tail<3>().transpose()<<"\n";
}
int main(int argc,char** argv) {
    if(argc<3) return 2;
    PoseHistory history;for(auto p:read(argv[2])) history.add(p.time,p.pose);
    std::vector<Sample> samples;int lag=argc>3?atoi(argv[3]):-10;
    for(auto& p:read(argv[1])) {Pose q;if(history.at(p.time+int64_t(lag)*1000000,q)) samples.push_back({q,p.pose});}
    evaluate(samples,solve(samples,false).questFromLighthouse.rotation(),"Motoc centered axes");
    evaluate(samples,fitAxes(samples,0),"Uncentered unit axes");
    evaluate(samples,fitAxes(samples,1),"Uncentered rotation vectors");
}
