// SPDX-License-Identifier: MPL-2.0
import SwiftUI
import ScreenCaptureKit
import AVFoundation
import CoreAudio
import AudioToolbox
import Darwin

enum AudioDiagnostic {
    static let lock=NSLock()
    static var seen=Set<String>()
    static func once(_ key:String,_ message:String){
        lock.lock();defer{lock.unlock()};guard seen.insert(key).inserted else{return}
        let path=FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Documents/VRCFaceTracking/audio-control.log")
        if !FileManager.default.fileExists(atPath:path.path){FileManager.default.createFile(atPath:path.path,contents:nil)}
        if let file=try? FileHandle(forWritingTo:path){try? file.seekToEnd();try? file.write(contentsOf:Data((message+"\n").utf8));try? file.close()}
    }
}
final class MicBuffer: @unchecked Sendable {
    let lock = NSLock()
    var data = [Int16](repeating: 0, count: 9600)
    var read = 0, write = 0, count = 0
    var primed = false
    var renderedFrames:UInt64=0,peak:Int=0
    func push(_ samples: [Int16]) {
        lock.lock(); defer { lock.unlock() }
        for sample in samples { if count == data.count { read = (read + 1) % data.count; count -= 1 }; data[write] = sample; write = (write+1) % data.count; count += 1 }
    }
    func render(_ frames: Int, _ list: UnsafeMutableAudioBufferListPointer) {
        for b in list { if let p=b.mData { memset(p,0,Int(b.mDataByteSize)) } }
        guard lock.try() else { return }; defer { lock.unlock() }
        if count > 2400 { let drop=count-960; read=(read+drop)%data.count; count-=drop }
        if !primed && count >= 960 { primed=true }
        guard primed else { return }
        for i in 0..<frames {
            guard count>0 else { primed=false; break }
            peak=max(peak,abs(Int(data[read])));renderedFrames &+= 1
            let value=Float(data[read])/32768; read=(read+1)%data.count;count-=1
            for b in list { guard let pointer=b.mData else { continue };let p=pointer.assumingMemoryBound(to: Float.self); for c in 0..<Int(b.mNumberChannels) { p[i*Int(b.mNumberChannels)+c]=value } }
        }
    }
}
final class MediaLink: @unchecked Sendable {
    let lock=NSLock(), mic=MicBuffer()
    var fd:Int32 = -1, token:UInt64=0, sequence:UInt32=0
    var received:UInt64=0, sent:UInt64=0
    var microphoneRequested=false,headsetConnected=false
    var lastMicSequence:UInt32?=nil
    private func header(_ type:UInt16, frames:UInt32=0, channels:UInt16=0, bytes:UInt16=0) -> Data {
        var d=Data();func append<T>(_ value:T){var v=value;withUnsafeBytes(of:&v){d.append(contentsOf:$0)}}
        append(UInt32(0x314d4356));append(UInt16(1));append(type);append(sequence);sequence &+= 1;append(frames);append(UInt32(48000));append(channels);append(bytes);append(token);return d
    }
    func start() throws {
        let s=socket(AF_INET,SOCK_DGRAM,0);guard s>=0 else { throw NSError(domain:"Audio socket",code:1) }
        var addr=sockaddr_in();addr.sin_len=UInt8(MemoryLayout<sockaddr_in>.size);addr.sin_family=sa_family_t(AF_INET);addr.sin_port=UInt16(47900).bigEndian;addr.sin_addr.s_addr=inet_addr("127.0.0.1")
        let result=withUnsafePointer(to:&addr){ $0.withMemoryRebound(to:sockaddr.self,capacity:1){ Darwin.connect(s,$0,socklen_t(MemoryLayout<sockaddr_in>.size)) } }
        guard result==0 else {close(s);throw NSError(domain:"Audio connection",code:Int(errno))}
        var timeout=timeval(tv_sec:0,tv_usec:20000);setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&timeout,socklen_t(MemoryLayout<timeval>.size))
        lock.lock();fd=s;token=0;lock.unlock()
        DispatchQueue.global(qos:.userInteractive).async { [self] in
            var hello=Date.distantPast;var buffer=[UInt8](repeating:0,count:1056)
            while true {
                lock.lock();let alive=fd==s
                if alive && Date().timeIntervalSince(hello)>1 {let d=header(7,frames:microphoneRequested ? 1:0);d.withUnsafeBytes{_ = Darwin.send(s,$0.baseAddress,d.count,0)};hello=Date()}
                lock.unlock();if !alive {break}
                let n=recv(s,&buffer,buffer.count,0);if n<32 {continue}
                let packet=Data(buffer.prefix(n))
                func value<T>(_ offset:Int,_ type:T.Type)->T{packet.withUnsafeBytes{$0.loadUnaligned(fromByteOffset:offset,as:T.self)}}
                if value(0,UInt32.self) != 0x314d4356 || value(4,UInt16.self) != 1 {continue}
                let type=value(6,UInt16.self), t=value(24,UInt64.self)
                lock.lock()
                if type==6 && n==32 {if token != t {lastMicSequence=nil}; token=t;headsetConnected=value(12,UInt32.self)==1}
                else if type==3 && t==token && token != 0 && n==512 && value(12,UInt32.self)==240 && value(16,UInt32.self)==48000 && value(20,UInt16.self)==1 && value(22,UInt16.self)==480 {
                    let seq=value(8,UInt32.self)
                    if lastMicSequence==nil || Int32(bitPattern:seq &- lastMicSequence!)>0 {
                        lastMicSequence=seq;received &+= 1
                        let samples=(0..<240).map{value(32+$0*2,Int16.self)};mic.push(samples)
                    }
                }
                lock.unlock()
            }
            close(s)
        }
    }
    func sendAudio(_ samples:[Int16]) {
        guard samples.count==480 else{return}
        lock.lock();defer{lock.unlock()};guard fd>=0 && token != 0 else{return}
        var d=header(2,frames:240,channels:2,bytes:960);samples.withUnsafeBytes{d.append(contentsOf:$0)};d.withUnsafeBytes{_ = Darwin.send(fd,$0.baseAddress,d.count,0)};sent &+= 1
    }
    func requestMicrophone(){lock.lock();microphoneRequested=true;lock.unlock()}
    func status()->(UInt64,UInt64,Bool,Bool){lock.lock();defer{lock.unlock()};return(sent,received,token != 0,headsetConnected)}
    func stop(){lock.lock();fd = -1;token=0;microphoneRequested=false;headsetConnected=false;lock.unlock()}
}
func findMicrophone() -> AudioDeviceID? {
    var address=AudioObjectPropertyAddress(mSelector:kAudioHardwarePropertyDevices,mScope:kAudioObjectPropertyScopeGlobal,mElement:kAudioObjectPropertyElementMain)
    var size:UInt32=0;guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject),&address,0,nil,&size)==noErr else{return nil}
    var ids=[AudioDeviceID](repeating:0,count:Int(size)/MemoryLayout<AudioDeviceID>.size)
    guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject),&address,0,nil,&size,&ids)==noErr else{return nil}
    for id in ids {var a=AudioObjectPropertyAddress(mSelector:kAudioDevicePropertyDeviceUID,mScope:kAudioObjectPropertyScopeGlobal,mElement:kAudioObjectPropertyElementMain);var uid:CFString="" as CFString;var n=UInt32(MemoryLayout<CFString>.size);if AudioObjectGetPropertyData(id,&a,0,nil,&n,&uid)==noErr && (uid as String)=="VRConMac Microphone_UID" {return id}}
    return nil
}
@MainActor final class AudioControl:NSObject,ObservableObject,SCStreamOutput,SCStreamDelegate {
    @Published var message="Start audio after the patched VR runtime is running."
    @Published var running=false
    @Published var counts=""
    static weak var current:AudioControl?
    override init(){super.init();AudioControl.current=self}
    let link=MediaLink()
    var stream:SCStream?, engine:AVAudioEngine?, source:AVAudioSourceNode?, previousInput:AudioDeviceID?, microphoneID:AudioDeviceID?
    var pending=[Int16]()
    let captureQueue=DispatchQueue(label:"net.vrconmac.capture",qos:.userInteractive)
    var timer:Timer?
    func start() async {
        guard !running else{return}
        do {
            let content=try await SCShareableContent.excludingDesktopWindows(false,onScreenWindowsOnly:false)
            guard let display=content.displays.first else{throw NSError(domain:"No display available",code:1)}
            let own=content.applications.filter{$0.processID==ProcessInfo.processInfo.processIdentifier}
            let filter=SCContentFilter(display:display,excludingApplications:own,exceptingWindows:[])
            let config=SCStreamConfiguration();config.width=2;config.height=2;config.minimumFrameInterval=CMTime(value:1,timescale:1);config.capturesAudio=true;config.excludesCurrentProcessAudio=true;config.sampleRate=48000;config.channelCount=2
            let newStream=SCStream(filter:filter,configuration:config,delegate:self)
            try newStream.addStreamOutput(self,type:.audio,sampleHandlerQueue:captureQueue)
            try link.start();stream=newStream
            try await newStream.startCapture();running=true
            message="Mac audio is streaming. Enable the headset microphone below."
            timer=Timer.scheduledTimer(withTimeInterval:1,repeats:true){ [weak self] _ in Task{@MainActor in guard let self else{return};let (up,down,connected,headset)=self.link.status();self.link.mic.lock.lock();let rendered=self.link.mic.renderedFrames,peak=self.link.mic.peak;self.link.mic.lock.unlock();if rendered>0{AudioDiagnostic.once("render", "Microphone rendered=\(rendered), peak=\(peak)")}
                self.counts="Runtime: \(connected ? "connected" : "waiting") · headset: \(headset ? "connected" : "waiting") · audio \(up) · mic \(down) packets"}}
        }catch{link.stop();stream=nil;message="Audio could not start: \(error.localizedDescription). Allow Screen & System Audio Recording in System Settings, then restart this app."}
    }
    func enableMic(){
        guard let device=findMicrophone() else{message="Install VRConMac Microphone.driver, then restart the Mac and reopen this app.";return}
        do {
            let e=AVAudioEngine();guard let unit=e.outputNode.audioUnit else{throw NSError(domain:"Audio output unavailable",code:1)}
            var selected=device;let status=AudioUnitSetProperty(unit,kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,&selected,UInt32(MemoryLayout<AudioDeviceID>.size))
            guard status==noErr else{throw NSError(domain:"Selecting virtual microphone",code:Int(status))}
            let ring=link.mic;let node=AVAudioSourceNode(format:AVAudioFormat(standardFormatWithSampleRate:48000,channels:2)!){silence,_,frames,list in silence.pointee=false; ring.render(Int(frames),UnsafeMutableAudioBufferListPointer(list));return noErr}
            e.attach(node);e.connect(node,to:e.outputNode,format:AVAudioFormat(standardFormatWithSampleRate:48000,channels:2));try e.start()
            var actual=AudioDeviceID(0),actualSize=UInt32(MemoryLayout<AudioDeviceID>.size)
            let getStatus=AudioUnitGetProperty(unit,kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,&actual,&actualSize)
            AudioDiagnostic.once("device", "Virtual output requested=\(device), actual=\(actual), status=\(getStatus)")
            var address=AudioObjectPropertyAddress(mSelector:kAudioHardwarePropertyDefaultInputDevice,mScope:kAudioObjectPropertyScopeGlobal,mElement:kAudioObjectPropertyElementMain)
            var old=AudioDeviceID(0),size=UInt32(MemoryLayout<AudioDeviceID>.size)
            if AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject),&address,0,nil,&size,&old)==noErr {previousInput=old}
            guard AudioObjectSetPropertyData(AudioObjectID(kAudioObjectSystemObject),&address,0,nil,size,&selected)==noErr else{e.stop();throw NSError(domain:"Selecting default microphone",code:1)}
            engine=e;source=node;microphoneID=device;link.requestMicrophone();message="Headset microphone is now the Mac input. Start VRChat after enabling it."
        } catch{message="Microphone could not start: \(error.localizedDescription)"}
    }
    func stop() async {
        timer?.invalidate();timer=nil
        if let stream {try? await stream.stopCapture()};stream=nil;link.stop();engine?.stop();engine=nil;source=nil
        if var previous=previousInput,let ours=microphoneID {var address=AudioObjectPropertyAddress(mSelector:kAudioHardwarePropertyDefaultInputDevice,mScope:kAudioObjectPropertyScopeGlobal,mElement:kAudioObjectPropertyElementMain);var current=AudioDeviceID(0),size=UInt32(MemoryLayout<AudioDeviceID>.size);if AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject),&address,0,nil,&size,&current)==noErr && current==ours {_ = AudioObjectSetPropertyData(AudioObjectID(kAudioObjectSystemObject),&address,0,nil,size,&previous)}}
        previousInput=nil;microphoneID=nil;running=false;counts="";message="Stopped. Previous Mac microphone restored."
    }
    nonisolated func stream(_ stream:SCStream,didStopWithError error:Error){Task{@MainActor in self.message="Capture stopped: \(error.localizedDescription)";await self.stop()}}
    nonisolated func stream(_ stream:SCStream,didOutputSampleBuffer sample:CMSampleBuffer,of type:SCStreamOutputType){
        AudioDiagnostic.once("callback", "Capture callback: type=\(type.rawValue), valid=\(sample.isValid)")
        guard type == .audio, sample.isValid, let desc=sample.formatDescription,let asbd=CMAudioFormatDescriptionGetStreamBasicDescription(desc) else{AudioDiagnostic.once("noformat", "Missing valid audio format");return}
        let format=asbd.pointee;AudioDiagnostic.once("format", "Capture format: rate=\(format.mSampleRate) bits=\(format.mBitsPerChannel) flags=\(format.mFormatFlags) channels=\(format.mChannelsPerFrame)");guard format.mSampleRate==48000 && format.mBitsPerChannel==32 && (format.mFormatFlags & kAudioFormatFlagIsFloat) != 0 else{return}
        var required=0
        let sizeStatus=CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample,bufferListSizeNeededOut:&required,bufferListOut:nil,bufferListSize:0,blockBufferAllocator:nil,blockBufferMemoryAllocator:nil,flags:0,blockBufferOut:nil)
        AudioDiagnostic.once("size", "Audio buffer list needed=\(required), query status=\(sizeStatus)")
        guard required>0 && required<=1048576 else{return}
        let memory=UnsafeMutableRawPointer.allocate(byteCount:required,alignment:16)
        defer{memory.deallocate()}
        let storage=memory.bindMemory(to:AudioBufferList.self,capacity:1)
        var block:CMBlockBuffer?
        let bufferStatus=CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample,bufferListSizeNeededOut:nil,bufferListOut:storage,bufferListSize:required,blockBufferAllocator:nil,blockBufferMemoryAllocator:nil,flags:0,blockBufferOut:&block)
        AudioDiagnostic.once("buffer", "Audio buffer extraction status=\(bufferStatus)")
        guard bufferStatus==noErr else{return}
        let buffers=UnsafeMutableAudioBufferListPointer(storage);let frames=CMSampleBufferGetNumSamples(sample)
        AudioDiagnostic.once("layout", "Audio list buffers=\(buffers.count), frames=\(frames)")
        guard buffers.count>0 else{return}
        var samples=[Int16]();samples.reserveCapacity(frames*2)
        for i in 0..<frames {for c in 0..<2 {let b=buffers[min(c,buffers.count-1)];guard let p=b.mData?.assumingMemoryBound(to:Float.self) else{return};let index=i*Int(b.mNumberChannels)+(buffers.count==1 ? min(c,Int(b.mNumberChannels)-1):0);guard index<Int(b.mDataByteSize)/MemoryLayout<Float>.size else{AudioDiagnostic.once("short-buffer", "Audio buffer shorter than its reported sample count");return};let value=p[index];samples.append(Int16(max(-32768,min(32767,value.isFinite ? value*32767:0))))}}
        // This delegate runs on a serial queue; packet assembly never touches the XR frame loop.
        capturePending.append(contentsOf:samples)
        while capturePending.count>=480 {link.sendAudio(Array(capturePending.prefix(480)));capturePending.removeFirst(480)}
    }
    nonisolated(unsafe) private var capturePending=[Int16]()
}
final class AudioDelegate:NSObject,NSApplicationDelegate {
    func applicationShouldTerminate(_ sender:NSApplication)->NSApplication.TerminateReply {
        Task{@MainActor in await AudioControl.current?.stop();sender.reply(toApplicationShouldTerminate:true)}
        return .terminateLater
    }
}
@main struct AudioApp:App {
    @NSApplicationDelegateAdaptor(AudioDelegate.self) var delegate

    @StateObject var control=AudioControl()
    var body:some Scene {WindowGroup("VRConMac Audio") {VStack(alignment:.leading,spacing:18){Text("Headset audio & microphone").font(.title2);Text(control.message).fixedSize(horizontal:false,vertical:true);Text(control.counts).font(.caption).foregroundStyle(.secondary);HStack{Button("Start headset audio"){Task{await control.start()}}.disabled(control.running);Button("Use headset microphone"){control.enableMic()}.disabled(!control.running || control.engine != nil);Button("Stop"){Task{await control.stop()}}.disabled(!control.running)};Text("Mac sound is sent to the Quest over your local network. The microphone returns through VRConMac Microphone. Close other sound sources if you only want VRChat audio.").font(.caption).foregroundStyle(.secondary)}.padding(24).frame(width:560).onReceive(NotificationCenter.default.publisher(for:NSApplication.willTerminateNotification)){_ in control.link.stop()}}}
}
