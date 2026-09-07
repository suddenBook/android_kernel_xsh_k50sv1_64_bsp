#!/usr/bin/env python3
"""Validate actual WLAN/P2P ARP address-list handlers and their emitted commands."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('scratch',type=Path)
parser.add_argument('--root',type=Path)
args=parser.parse_args()
root=args.root or Path(__file__).resolve().parents[3]
wlan=root/'drivers/misc/mediatek/connectivity/source/wlan/core/gen2'
def function(s,name):
 start=s.index('WLAN_STATUS\n'+name+'(')
 end=s.index('{',start)+1;depth=1
 while depth:
  depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]+'\n'
def section(path,start,end):
 s=(wlan/path).read_text(encoding='latin1');a=s.index(start);return s[a:s.index(end,a)]
oid=(wlan/'common/wlan_oid.c').read_text();p2p=(wlan/'common/wlan_p2p.c').read_text()
functions=''
if 'WLAN_STATUS\nwlanSetNetworkAddressList(' in oid: functions+=function(oid,'wlanSetNetworkAddressList')
functions+=function(oid,'wlanoidSetNetworkAddress')+function(p2p,'wlanoidSetP2pSetNetworkAddress')
structures=section('include/wlan_oid.h','typedef struct _PARAM_NETWORK_ADDRESS_IP {','\n#if CFG_SLT_SUPPORT')
structures+=section('include/nic_cmd_event.h','typedef struct _IPV4_NETWORK_ADDRESS {','\ntypedef struct _PATTERN_DESCRIPTION')
structures+=section('include/mgmt/hs20.h','typedef struct _IPV4_NETWORK_ADDRESS_LIST {','\n#endif')
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t UINT_8,*PUINT_8;
typedef uint16_t UINT_16;
typedef int16_t INT_16;
typedef uint32_t UINT_32,*PUINT_32,WLAN_STATUS;
typedef uintptr_t ULONG;
typedef void *PVOID;
#define IN
#define OUT
#define TRUE true
#define FALSE false
#define WLAN_STATUS_SUCCESS 0
#define WLAN_STATUS_FAILURE 1
#define WLAN_STATUS_INVALID_DATA 2
#define WLAN_STATUS_PENDING 3
#define PARAM_PROTOCOL_ID_TCP_IP 2
#define NETWORK_TYPE_AIS_INDEX 0
#define NETWORK_TYPE_P2P_INDEX 1
#define CMD_ID_SET_IP_ADDRESS 77
#define VIR_MEM_TYPE 1
#define OFFSET_OF(t,f) offsetof(t,f)
#define ASSERT(x) ((void)0)
#define DEBUGFUNC(x) ((void)0)
static void ignore_log(const char *fmt, ...) { (void)fmt; }
#define DBGLOG(m,c,...) ignore_log(__VA_ARGS__)
#define kalMemCopy(p,q,n) memcpy(p,q,n)
#define kalMemZero(p,n) memset(p,0,n)
'''
mocks=r'''
typedef struct { P_IPV4_NETWORK_ADDRESS_LIST prIpV4NetAddrList; } BSS_INFO_T,*P_BSS_INFO_T;
typedef struct { bool fgEnArpFilter; struct { BSS_INFO_T arBssInfo[2]; } rWifiVar; } ADAPTER_T,*P_ADAPTER_T;
static unsigned allocation_calls, free_calls, fail_allocation, live, commands;
static unsigned expected_count, expected_network;
static bool expected_filter;
static WLAN_STATUS command_status=WLAN_STATUS_PENDING;
static void *kalMemAlloc(size_t n,int kind) {
 assert(kind==VIR_MEM_TYPE && n>0 && n<8192);
 if (++allocation_calls==fail_allocation) return NULL;
 void *p=malloc(n);assert(p);memset(p,0xa5,n);++live;return p;
}
static void kalMemFree(void *p,int kind,size_t n) {
 (void)n;assert(kind==VIR_MEM_TYPE);
 if (p) {assert(live);--live;++free_calls;free(p);}
}
#define FREE_IPV4_NETWORK_ADDR_LIST(p) do {kalMemFree(p,VIR_MEM_TYPE,0);(p)=NULL;} while(0)
static void nicCmdEventSetIpAddress(void) {}
static void nicOidCmdTimeoutCommon(void) {}
static WLAN_STATUS wlanSendSetQueryCmd(P_ADAPTER_T adapter,UINT_8 id,bool set,bool response,
 bool is_oid,void (*done)(void),void (*timeout)(void),UINT_32 n,PUINT_8 bytes,void *source,UINT_32 size) {
 (void)source;(void)size;assert(adapter && id==CMD_ID_SET_IP_ADDRESS && set && !response && is_oid);
 assert(done==nicCmdEventSetIpAddress && timeout==nicOidCmdTimeoutCommon);
 CMD_SET_NETWORK_ADDRESS_LIST *cmd=(void *)bytes;
 assert(cmd->ucNetTypeIndex==expected_network && cmd->ucAddressCount==(expected_filter?expected_count:0));
 assert(n==(expected_count?offsetof(CMD_SET_NETWORK_ADDRESS_LIST,arNetAddress)+4*expected_count:sizeof(*cmd)));
 if(expected_filter) for(unsigned i=0;i<expected_count;i++) {
  UINT_32 ip;memcpy(&ip,cmd->arNetAddress[i].aucIpAddr,4);assert(ip==0x010200c0u+i);
 }
 ++commands;return command_status;
}
'''
tests=r'''
static size_t append_record(unsigned char *buf,size_t pos,UINT_16 type,UINT_16 length,UINT_32 ip) {
 memcpy(buf+pos,&length,2);memcpy(buf+pos+2,&type,2);memset(buf+pos+4,0,length);
 if(type==PARAM_PROTOCOL_ID_TCP_IP && length==sizeof(PARAM_NETWORK_ADDRESS_IP))
  memcpy(buf+pos+4+offsetof(PARAM_NETWORK_ADDRESS_IP,in_addr),&ip,4);
 return pos+4+length;
}
int main(int argc,char **argv) {
 assert(argc==3);bool p2p=atoi(argv[1]);const char *name=argv[2];
 ADAPTER_T adapter={.fgEnArpFilter=true};unsigned count=1;bool mixed=false,invalid=false;
 if(!strcmp(name,"empty"))count=0;
 else if(!strcmp(name,"two")||!strcmp(name,"filter-disabled"))count=2;
 else if(!strcmp(name,"mixed-odd")){count=2;mixed=true;}
 else if(!strcmp(name,"max"))count=255;
 else if(!strcmp(name,"too-many")){count=256;invalid=true;}
 else if(!strcmp(name,"short-header")||!strcmp(name,"short-record")||!strcmp(name,"short-payload")||
         !strcmp(name,"huge-count")||!strcmp(name,"huge-record")||!strcmp(name,"null-input")||
         !strcmp(name,"null-output")||!strcmp(name,"null-adapter"))invalid=true;
 else if(!strcmp(name,"oom"))fail_allocation=1;
 else if(!strcmp(name,"cache-oom")||!strcmp(name,"replace-oom"))fail_allocation=2;
 else if(!strcmp(name,"dispatch-error"))command_status=WLAN_STATUS_FAILURE;
 else assert(!strcmp(name,"one"));
 if(!strcmp(name,"filter-disabled"))adapter.fgEnArpFilter=false;
 unsigned char *data=calloc(1,6000);assert(data);size_t n=offsetof(PARAM_NETWORK_ADDRESS_LIST,arAddress);
 unsigned total=count+(mixed?1:0);memcpy(data,&total,4);
 UINT_16 type=PARAM_PROTOCOL_ID_TCP_IP;memcpy(data+4,&type,2);
 if(mixed)n=append_record(data,n,99,1,0);
 for(unsigned i=0;i<count;i++)n=append_record(data,n,PARAM_PROTOCOL_ID_TCP_IP,sizeof(PARAM_NETWORK_ADDRESS_IP),0x010200c0u+i);
 if(!count)n=sizeof(PARAM_NETWORK_ADDRESS_LIST);
 if(!strcmp(name,"short-header"))n=sizeof(PARAM_NETWORK_ADDRESS_LIST)-1;
 if(!strcmp(name,"short-record")){total=2;memcpy(data,&total,4);n+=2;}
 if(!strcmp(name,"short-payload"))--n;
 if(!strcmp(name,"huge-count")){total=0xffffffff;memcpy(data,&total,4);}
 if(!strcmp(name,"huge-record")){UINT_16 length=65535;memcpy(data+offsetof(PARAM_NETWORK_ADDRESS_LIST,arAddress),&length,2);}
 unsigned char *input=malloc(n);assert(input);memcpy(input,data,n);free(data);
 expected_count=count;expected_filter=adapter.fgEnArpFilter;expected_network=p2p?NETWORK_TYPE_P2P_INDEX:NETWORK_TYPE_AIS_INDEX;
 UINT_32 info=999;P_IPV4_NETWORK_ADDRESS_LIST previous=NULL;
 if(!p2p && CFG_ENABLE_GTK_FRAME_FILTER && !strcmp(name,"replace-oom")) {
  previous=kalMemAlloc(sizeof(*previous),VIR_MEM_TYPE);previous->ucAddrCount=1;
  adapter.rWifiVar.arBssInfo[0].prIpV4NetAddrList=previous;allocation_calls=0;
 }
 bool allocation_error=fail_allocation==1 || (fail_allocation==2 && CFG_ENABLE_GTK_FRAME_FILTER && !p2p);
 WLAN_STATUS (*handler)(P_ADAPTER_T,PVOID,UINT_32,PUINT_32)=p2p?wlanoidSetP2pSetNetworkAddress:wlanoidSetNetworkAddress;
 WLAN_STATUS status=handler(!strcmp(name,"null-adapter")?NULL:&adapter,
     !strcmp(name,"null-input")?NULL:input,(UINT_32)n,!strcmp(name,"null-output")?NULL:&info);
 if(invalid){assert(status==WLAN_STATUS_INVALID_DATA && !commands && !allocation_calls);}
 else if(allocation_error){assert(status==WLAN_STATUS_FAILURE && !commands);}
 else {assert(status==command_status && commands==1);}
 if(CFG_ENABLE_GTK_FRAME_FILTER && !p2p) {
  P_IPV4_NETWORK_ADDRESS_LIST cache=adapter.rWifiVar.arBssInfo[0].prIpV4NetAddrList;
  if(invalid || allocation_error || command_status==WLAN_STATUS_FAILURE)assert(cache==previous);
  else {
   assert(cache && cache->ucAddrCount==count);
   if(expected_filter)for(unsigned i=0;i<count;i++) {
    UINT_32 ip;memcpy(&ip,cache->arNetAddr[i].aucIpAddr,4);assert(ip==0x010200c0u+i);
   }
  }
  if(cache)FREE_IPV4_NETWORK_ADDR_LIST(cache);
 }
 assert(live==0);free(input);
 printf("PASS %s/%s GTK=%d\n",p2p?"P2P":"STA",name,CFG_ENABLE_GTK_FRAME_FILTER);
}
'''
cases=('empty','one','two','mixed-odd','max','too-many','filter-disabled','short-header',
       'short-record','short-payload','huge-count','huge-record','null-input','null-output',
       'null-adapter','oom','cache-oom','replace-oom','dispatch-error')
args.scratch.mkdir(parents=True,exist_ok=True)
failed=[]
with tempfile.TemporaryDirectory(prefix='network-address-',dir=args.scratch) as directory:
 unit=Path(directory)/'test.c';unit.write_text(prefix+structures+mocks+functions+tests)
 for gtk in (0,1):
  binary=Path(directory)/f'test-{gtk}'
  subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function',
                  f'-DCFG_ENABLE_GTK_FRAME_FILTER={gtk}','-fsanitize=address,undefined',
                  '-fno-sanitize-recover=all',str(unit),'-o',str(binary)],check=True)
  for p2p in (0,1):
   for name in cases:
    result=subprocess.run([str(binary),str(p2p),name],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    if result.returncode:
     failed.append((gtk,p2p,name));print(f'FAIL GTK={gtk} P2P={p2p} {name}: {result.stdout.splitlines()[0] if result.stdout else result.returncode}')
    else:print(result.stdout,end='')
print(f'{4*len(cases)-len(failed)}/{4*len(cases)} cases pass; failures={failed}')
raise SystemExit(bool(failed))
