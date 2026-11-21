// SPDX-License-Identifier: MPL-2.0
using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using System.Diagnostics;
using VRCFaceTracking;
using VRCFaceTracking.Core.Types;
namespace VRConMac.Tracking;
public sealed class QuestModule:ExtTrackingModule {
 readonly int port;
 public QuestModule():this(47901){}
 public QuestModule(int listenPort){port=listenPort;}
 UdpClient? client;readonly Stopwatch age=Stopwatch.StartNew();bool eye,face;uint? sequence;
 public override (bool SupportsEye,bool SupportsExpression) Supported=>(true,true);
 public override (bool eyeSuccess,bool expressionSuccess) Initialize(bool eyeAvailable,bool expressionAvailable){
  ModuleInformation.Name="VRConMac Quest Pro";eye=eyeAvailable;face=expressionAvailable;
  try{client=new UdpClient(new IPEndPoint(IPAddress.Loopback,port));client.Client.ReceiveTimeout=10;Neutral();ModuleInformation.Active=false;return(eye,face);}catch(SocketException){return(false,false);}
 }
 public static bool Validate(ReadOnlySpan<byte> packet){
  if(packet.Length!=368||BinaryPrimitives.ReadUInt32LittleEndian(packet)!=0x314d4356||BinaryPrimitives.ReadUInt16LittleEndian(packet[4..])!=1||BinaryPrimitives.ReadUInt16LittleEndian(packet[6..])!=4||BinaryPrimitives.ReadUInt16LittleEndian(packet[22..])!=336)return false;
  var body=packet[32..];if((BinaryPrimitives.ReadUInt32LittleEndian(body)&~3u)!=0||BinaryPrimitives.ReadUInt32LittleEndian(body[4..])!=70)return false;
  for(int i=16;i<336;i+=4){float f=BitConverter.Int32BitsToSingle(BinaryPrimitives.ReadInt32LittleEndian(body[i..]));if(!float.IsFinite(f)||(i>=48&&(f<0||f>1)))return false;}
  return true;
 }
 public static Vector2 Gaze(float x,float y,float z,float w){
  float n=MathF.Sqrt(x*x+y*y+z*z+w*w);if(n<0.001f)return new Vector2(0,0);x/=n;y/=n;z/=n;w/=n;
  return new Vector2(MathF.Asin(Math.Clamp(2*(x*z-w*y),-1,1)),MathF.Atan2(2*(y*z+w*x),w*w-x*x-y*y+z*z));
 }
 public static void Apply(ReadOnlySpan<byte> packet,bool useEye=true,bool useFace=true){
  var body=packet[32..];uint flags=BinaryPrimitives.ReadUInt32LittleEndian(body);
  var values=packet[32..].ToArray();
  float F(int offset)=>BitConverter.Int32BitsToSingle(BinaryPrimitives.ReadInt32LittleEndian(values.AsSpan(offset)));
  var data=UnifiedTracking.Data;
  if(useEye){if((flags&2)!=0){data.Eye.Left.Gaze=Gaze(F(16),F(20),F(24),F(28));data.Eye.Right.Gaze=Gaze(F(32),F(36),F(40),F(44));}else{data.Eye.Left.Gaze=new Vector2(0,0);data.Eye.Right.Gaze=new Vector2(0,0);}}
  if(useFace){var weights=new float[70];if((flags&1)!=0)for(int i=0;i<70;i++)weights[i]=F(56+4*i);FaceMapping.UpdateEyeExpressionsFBV2(ref data.Shapes,weights);FaceMapping.UpdateMouthExpressionsFBV2(ref data.Shapes,weights);if(useEye){data.Eye.Left.Openness=1-Math.Clamp(weights[12]*(1+weights[28]),0,1);data.Eye.Right.Openness=1-Math.Clamp(weights[13]*(1+weights[29]),0,1);}}
 }
 public override void Update(){
  if(client==null){Thread.Sleep(10);return;}
  try{IPEndPoint endpoint=new(IPAddress.Loopback,0);byte[]? latest=null;
   do{var p=client.Receive(ref endpoint);if(!IPAddress.IsLoopback(endpoint.Address)||!Validate(p))continue;uint s=BinaryPrimitives.ReadUInt32LittleEndian(p.AsSpan(8));if(sequence is not null&&unchecked((int)(s-sequence.Value))<=0)continue;sequence=s;latest=p;}while(client.Available>0);
   if(latest!=null){Apply(latest,eye,face);age.Restart();ModuleInformation.Active=true;}
  }catch(SocketException e)when(e.SocketErrorCode==SocketError.TimedOut||e.SocketErrorCode==SocketError.WouldBlock){}
  if(age.ElapsedMilliseconds>1000){Neutral();sequence=null;ModuleInformation.Active=false;}
 }
 void Neutral(){if(face)for(int i=0;i<UnifiedTracking.Data.Shapes.Length;i++)UnifiedTracking.Data.Shapes[i].Weight=0;if(eye){UnifiedTracking.Data.Eye.Left.Gaze=new Vector2(0,0);UnifiedTracking.Data.Eye.Right.Gaze=new Vector2(0,0);UnifiedTracking.Data.Eye.Left.Openness=1;UnifiedTracking.Data.Eye.Right.Openness=1;}}
 public override void Teardown(){client?.Dispose();client=null;Neutral();ModuleInformation.Active=false;}
}
