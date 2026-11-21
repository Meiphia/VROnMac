import socket,struct,subprocess,time
from pathlib import Path
H=struct.Struct('<IHHIIIHHQ');MAGIC=0x314d4356
server=subprocess.Popen([str(Path(__file__).resolve().parents[2] / 'build/audio-tracking/test-relay')],stdout=subprocess.PIPE,text=True)
assert server.stdout.readline().strip()=='READY'
time.sleep(.05)
def peer(kind):
 s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.bind(('127.0.0.1',0));s.settimeout(.4)
 s.sendto(H.pack(MAGIC,1,kind,0,1 if kind==7 else 0,48000,0,0,0),('127.0.0.1',47910));ack=s.recv(1056);assert H.unpack(ack)[2]==6;return s,H.unpack(ack)[-1]
mac,mt=peer(7);quest,qt=peer(1)
def audio(kind,token,seq=1):
 channels=2 if kind==2 else 1;pcm=bytes((i%256 for i in range(240*channels*2)));return H.pack(MAGIC,1,kind,seq,240,48000,channels,len(pcm),token)+pcm
packet=audio(2,mt);mac.sendto(packet,('127.0.0.1',47910));received=quest.recv(1056);assert received[32:]==packet[32:] and H.unpack(received[:32])[-1]==qt
packet=audio(3,qt);quest.sendto(packet,('127.0.0.1',47910));received=mac.recv(1056);assert received[32:]==packet[32:] and H.unpack(received[:32])[-1]==mt
for bad in [audio(2,mt^1),audio(2,mt)[:-1],H.pack(MAGIC,1,2,1,240,48000,2,960,mt)+bytes(962)]:
 mac.sendto(bad,('127.0.0.1',47910))
 try:quest.recv(1056);raise AssertionError('invalid packet forwarded')
 except socket.timeout:pass
tracking=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);tracking.bind(('127.0.0.1',47911));tracking.settimeout(.4)
body=struct.pack('<IIQ8f2f70f',3,70,100,0,0,0,1,0,0,0,1,.9,.9,*([0.0]*70))
packet=H.pack(MAGIC,1,4,7,0,48000,0,len(body),qt)+body;quest.sendto(packet,('127.0.0.1',47910));assert tracking.recv(1056)==packet
bad=bytearray(packet);struct.pack_into('<f',bad,88,float('nan'));quest.sendto(bad,('127.0.0.1',47910))
try:tracking.recv(1056);raise AssertionError('NaN accepted')
except socket.timeout:pass
server.wait();assert server.returncode==0
print('PASS: stereo audio, microphone, token checks, malformed sizes, face forwarding, NaN rejection, bounded shutdown')
