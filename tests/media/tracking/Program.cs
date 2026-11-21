using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using VRCFaceTracking;
using VRCFaceTracking.Core.Params.Expressions;
using VRConMac.Tracking;
static void Check(bool ok,string name){if(!ok)throw new Exception(name);}
var p=new byte[368];void U(int offset,uint v)=>BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(offset),v);void S(int offset,ushort v)=>BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(offset),v);void F(int offset,float v)=>U(offset,BitConverter.SingleToUInt32Bits(v));
U(0,0x314d4356);S(4,1);S(6,4);S(22,336);U(32,3);U(36,70);F(60,1);F(76,1);F(80,.9f);F(84,.9f);
F(88+24*4,.75f);F(88+32*4,.5f);F(88+12*4,1);
Check(QuestModule.Validate(p),"valid sample");QuestModule.Apply(p);
Check(Math.Abs(UnifiedTracking.Data.Shapes[(int)UnifiedExpressions.JawOpen].Weight-.75f)<.001,"jaw mapping");
Check(Math.Abs(UnifiedTracking.Data.Shapes[(int)UnifiedExpressions.MouthCornerPullLeft].Weight-.5f)<.001,"smile mapping");Check(UnifiedTracking.Data.Eye.Left.Openness==0,"blink mapping");
var gaze=QuestModule.Gaze(0,MathF.Sin(.1f),0,MathF.Cos(.1f));Check(Math.Abs(gaze.x+.2f)<.001,"normalized gaze angle");
F(88,float.NaN);Check(!QuestModule.Validate(p),"NaN rejected");F(88,0);S(22,335);Check(!QuestModule.Validate(p),"length rejected");S(22,336);
var module=new QuestModule(47911);Check(module.Initialize(true,true)==(true,true),"module initializes");
using var sender=new UdpClient();U(8,10);sender.Send(p,new IPEndPoint(IPAddress.Loopback,47911));module.Update();Check(UnifiedTracking.Data.Shapes[(int)UnifiedExpressions.JawOpen].Weight>.7,"UDP sample applied");U(8,9);F(88+24*4,.1f);sender.Send(p,new IPEndPoint(IPAddress.Loopback,47911));module.Update();Check(UnifiedTracking.Data.Shapes[(int)UnifiedExpressions.JawOpen].Weight>.7,"old sample rejected");Thread.Sleep(1050);module.Update();Check(UnifiedTracking.Data.Shapes[(int)UnifiedExpressions.JawOpen].Weight==0,"stale sample neutralized");module.Teardown();
Console.WriteLine("PASS: face, blink, gaze mapping; packet validation; UDP delivery; ordering; stale data reset");

Console.WriteLine("DATA="+VRCFaceTracking.Core.Utils.PersistentDataDirectory);Console.WriteLine("DOCUMENTS="+VRCFaceTracking.Core.Utils.UserAccessibleDataDirectory);

using (var message = new VRCFaceTracking.Core.OSC.OscMessage("/avatar/parameters/VRConMacTest", typeof(float)))
{
    message.Value = .75f;
    byte[] encoded = new byte[4096];
    int length = await message.Encode(encoded, CancellationToken.None);
    Check(length > 8 && System.Text.Encoding.UTF8.GetString(encoded, 0, length).StartsWith("/avatar/parameters/VRConMacTest"), "native ARM64 OSC encoder");
    Check(Math.Abs(BitConverter.Int32BitsToSingle(BinaryPrimitives.ReadInt32BigEndian(encoded.AsSpan(length - 4))) - .75f) < .001, "OSC float byte order");
    using var listener = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
    sender.Send(encoded.AsSpan(0, length).ToArray(), (IPEndPoint)listener.Client.LocalEndPoint!);
    var received = await listener.ReceiveAsync().WaitAsync(TimeSpan.FromSeconds(1));
    Check(received.Buffer.AsSpan().SequenceEqual(encoded.AsSpan(0, length)), "OSC UDP loopback");
}
Console.WriteLine("PASS: native ARM64 OSC serialization and UDP delivery");
