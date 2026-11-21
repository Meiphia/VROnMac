#include <mach-o/dyld.h>
#include <unistd.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char**argv){char path[PATH_MAX],real[PATH_MAX],executable[PATH_MAX];uint32_t n=sizeof(path);if(_NSGetExecutablePath(path,&n)||!realpath(path,real))return 1;char*slash=strrchr(real,'/');if(!slash)return 1;*slash=0;snprintf(path,sizeof(path),"%s/../Resources/vrcft",real);if(chdir(path)){perror("VRCFaceTracking app files");return 1;}snprintf(executable,sizeof(executable),"%s/VRCFaceTracking",path);argv[0]=executable;execv(executable,argv);perror("Launching VRCFaceTracking");return 1;}
