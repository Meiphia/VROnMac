import CoreMedia
import AudioToolbox
import Foundation
var format=AudioStreamBasicDescription(mSampleRate:48000,mFormatID:kAudioFormatLinearPCM,mFormatFlags:kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked|kAudioFormatFlagIsNonInterleaved,mBytesPerPacket:4,mFramesPerPacket:1,mBytesPerFrame:4,mChannelsPerFrame:2,mBitsPerChannel:32,mReserved:0)
var description:CMAudioFormatDescription?
precondition(CMAudioFormatDescriptionCreate(allocator:kCFAllocatorDefault,asbd:&format,layoutSize:0,layout:nil,magicCookieSize:0,magicCookie:nil,extensions:nil,formatDescriptionOut:&description)==noErr)
var block:CMBlockBuffer?
let frames=1024,bytes=frames*8
precondition(CMBlockBufferCreateWithMemoryBlock(allocator:kCFAllocatorDefault,memoryBlock:nil,blockLength:bytes,blockAllocator:kCFAllocatorDefault,customBlockSource:nil,offsetToData:0,dataLength:bytes,flags:0,blockBufferOut:&block)==noErr)
var samples=[Float](repeating:0.5,count:frames)+[Float](repeating:-0.5,count:frames)
precondition(CMBlockBufferReplaceDataBytes(with:&samples,blockBuffer:block!,offsetIntoDestination:0,dataLength:bytes)==noErr)
var sample:CMSampleBuffer?
precondition(CMAudioSampleBufferCreateReadyWithPacketDescriptions(allocator:kCFAllocatorDefault,dataBuffer:block!,formatDescription:description!,sampleCount:frames,presentationTimeStamp:.zero,packetDescriptions:nil,sampleBufferOut:&sample)==noErr)
var size=0
let query=CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample!,bufferListSizeNeededOut:&size,bufferListOut:nil,bufferListSize:0,blockBufferAllocator:nil,blockBufferMemoryAllocator:nil,flags:0,blockBufferOut:nil)
print("query=\(query), required=\(size), sizeof=\(MemoryLayout<AudioBufferList>.size)")
precondition(size>0)
let memory=UnsafeMutableRawPointer.allocate(byteCount:size,alignment:16);defer{memory.deallocate()}
var retained:CMBlockBuffer?
let result=CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample!,bufferListSizeNeededOut:nil,bufferListOut:memory.assumingMemoryBound(to:AudioBufferList.self),bufferListSize:size,blockBufferAllocator:nil,blockBufferMemoryAllocator:nil,flags:0,blockBufferOut:&retained)
print("extract=\(result)")
precondition(result==noErr)
let list=UnsafeMutableAudioBufferListPointer(memory.assumingMemoryBound(to:AudioBufferList.self))
precondition(list.count==2 && list[0].mDataByteSize==frames*4)
precondition(list[0].mData!.assumingMemoryBound(to:Float.self)[2]==0.5 && list[1].mData!.assumingMemoryBound(to:Float.self)[2]==(-0.5))
print("PASS: dynamically sized CoreMedia stereo buffers and plane samples")

let old=UnsafeMutablePointer<AudioBufferList>.allocate(capacity:8);defer{old.deallocate()}
var oldRetained:CMBlockBuffer?
let oldStatus=CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample!,bufferListSizeNeededOut:nil,bufferListOut:old,bufferListSize:MemoryLayout<AudioBufferList>.size*8,blockBufferAllocator:nil,blockBufferMemoryAllocator:nil,flags:0,blockBufferOut:&oldRetained)
print("old oversized buffer result=\(oldStatus)")
