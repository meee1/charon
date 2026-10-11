//MIT License
//
//Copyright (c) 2018 tvelliott
//
//Permission is hereby granted, free of charge, to any person obtaining a copy
//of this software and associated documentation files (the "Software"), to deal
//in the Software without restriction, including without limitation the rights
//to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//copies of the Software, and to permit persons to whom the Software is
//furnished to do so, subject to the following conditions:
//
//The above copyright notice and this permission notice shall be included in all
//copies or substantial portions of the Software.
//
//THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//SOFTWARE.

#define _POSIX_C_SOURCE 200112L
#define _GNU_SOURCE 1 
#include <netdb.h>

#include <stdio.h>
#include <unistd.h>
#include <string.h> 
#include <stdint.h>
#include <stdlib.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <ifaddrs.h>

#include "util.h"
#include "timers.h"
#include "config.h"


///////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////
uint32_t get_netif_ip(char *if_name, char *ip_a) {

uint32_t ip_addr;
struct ifaddrs *ifaddr;
struct ifaddrs *ifa;
struct sockaddr_in *sa;
int family;
int s;
char host[1024];
struct in_addr addr;

  if (getifaddrs(&ifaddr) == -1) {
    perror("getifaddrs");
    return 0xffffffff;
  }


  for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == NULL) continue;  

    s=getnameinfo(ifa->ifa_addr,sizeof(struct sockaddr_in),host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
      if (ifa==NULL) {
        return 0xffffffff;
      }

    if((strcmp(ifa->ifa_name,if_name)==0)&&(ifa->ifa_addr->sa_family==AF_INET)) {
      //printf("\tInterface : <%s>\n",ifa->ifa_name );
      //printf("\t  Address : <%s>\n", host); 
      strncpy(ip_a, host, INET_ADDRSTRLEN-1);
      ip_a[INET_ADDRSTRLEN-1] = '\0';
      inet_aton(host, &addr);

      //sa = (struct sockaddr_in *) ifa->ifa_addr;
      //if(sa!=NULL) {
      //  memcpy( &ip_addr, &sa->sin_addr, 4);
      //  freeifaddrs(ifaddr);
        //return htonl(ip_addr);
        return (uint32_t) addr.s_addr; 
      //}
    }
  }

  freeifaddrs(ifaddr);

  return 0x0102a8c0; 
}

////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////
static FILE *popen_env_file(const char *path) {
  char cmd[512];
  snprintf(cmd, sizeof(cmd), "cat '%s'", path);
  return popen(cmd, "r");
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
int read_fw_attr_value(const char*config_key, char *value, int max_len){

FILE * fp;
char line[1024];
char *ptr;
char *tok1;

  // CHARON_ENV_FILE (simulator / host runs): read key=value lines from a
  // file instead of the u-boot environment.  Same line format as
  // fw_printenv output.
  const char *env_file = getenv("CHARON_ENV_FILE");

  memset(line,0x00,sizeof(line));
  if((fp = env_file ? popen_env_file(env_file) : popen("/usr/sbin/fw_printenv","r")) != NULL){
    while(fgets(line,sizeof(line),fp)!=NULL){
      if(line[0]=='\n' || line[0]=='\r' || line[0]==' ') {
        memset(line,0x00,sizeof(line));
        continue;
      }

      //exact key match: "ipaddr" must not match "ipaddr_host=..."
      if( strncmp(config_key, &line[0], strlen(config_key))==0 &&
          (line[strlen(config_key)]=='=' || line[strlen(config_key)]==' ') ) {
        ptr = &line[strlen(config_key)];
        tok1 = strtok(ptr, " =\r\n");
        if(tok1 == NULL) {  //"key=" with no value: use the default
          fprintf(stderr, "\nfound %s key with empty value, ignoring", config_key);
          pclose(fp);
          return -1;
        }
        snprintf(value, max_len, "%s", tok1);

        fprintf(stderr, "\nfound %s key:  value:->%s<-", config_key, value);

        pclose(fp);
        return 0;
      }
      memset(line,0x00,sizeof(line));
    }
    goto clean; 
  } else {                    
    perror("read_config:fopen:");
    goto clean; 
  }

clean:
if(fp!=NULL) pclose(fp);
  return -1;
}
////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////
#define ROUTE_CACHE_TTL_USEC 2000000  // 2 seconds
static uint8_t cached_route[6];
static char    cached_mac[18];
static int     cache_valid = 0;
static timer_obj *route_cache_timer = NULL;

static int do_batman_route_lookup(const char*org_dest_mac_a, uint8_t *dst_route){

FILE * fp;
char line[1024];
char *ptr;
char *tok1;
char *tok2;
char *tok3;
char *tok4;

  memset(line,0x00,sizeof(line));
  if((fp = fopen("/sys/kernel/debug/batman_adv/bat0/originators","r")) != NULL){

    while(fgets(line,sizeof(line),fp)!=NULL){
      // skip header lines starting with '[' or "   Originator"
      if(line[0]=='[' || strncmp(line, "   Originator", 13)==0) {
        memset(line,0x00,sizeof(line));
        continue;
      }

      // data lines start with " * " for best route
      // format: " * 14:25:66:9b:f7:d9    0.600s   ( 26) 14:25:66:9b:f7:d9 [     ofdm0]"
      if(line[0]==' ' && line[1]=='*' && line[2]==' ') {
        ptr = &line[3]; // skip " * "

        tok1 = strtok(ptr, " "); // originator MAC
        if(tok1==NULL) goto clean;

        if( strncmp(org_dest_mac_a, tok1, strlen(org_dest_mac_a))!=0 ) {
          memset(line,0x00,sizeof(line));
          continue;
        }

        tok2 = strtok(NULL, " "); // last-seen time
        if(tok2==NULL) goto clean;

        tok3 = strtok(NULL, ")"); // TQ value with '('
        if(tok3==NULL) goto clean;

        tok4 = strtok(NULL, " "); // nexthop MAC
        if(tok4==NULL) goto clean;

        if( strlen(tok4) < 17 ) goto clean;

        fprintf(stderr, "\nnext hop-> %s", tok4);
        tok4[2] = 0;
        tok4[5] = 0;
        tok4[8] = 0;
        tok4[11] = 0;
        tok4[14] = 0;
        dst_route[0] = strtol(&tok4[0],NULL,16);
        dst_route[1] = strtol(&tok4[3],NULL,16);
        dst_route[2] = strtol(&tok4[6],NULL,16);
        dst_route[3] = strtol(&tok4[9],NULL,16);
        dst_route[4] = strtol(&tok4[12],NULL,16);
        dst_route[5] = strtol(&tok4[15],NULL,16);

        fclose(fp);
        return 1;
      }
      memset(line,0x00,sizeof(line));
    }
    goto clean;
  } else {
    // fallback to popen if debugfs not available
    if((fp = popen("batctl o","r")) != NULL){
      while(fgets(line,sizeof(line),fp)!=NULL){
        if(line[0]=='[' || strncmp(line, "   Originator", 13)==0) {
          memset(line,0x00,sizeof(line));
          continue;
        }
        if(line[0]==' ' && line[1]=='*' && line[2]==' ') {
          ptr = &line[3];
          tok1 = strtok(ptr, " ");
          if(tok1==NULL) { pclose(fp); return 0; }
          if( strncmp(org_dest_mac_a, tok1, strlen(org_dest_mac_a))!=0 ) {
            memset(line,0x00,sizeof(line));
            continue;
          }
          tok2 = strtok(NULL, " ");
          if(tok2==NULL) { pclose(fp); return 0; }
          tok3 = strtok(NULL, ")");
          if(tok3==NULL) { pclose(fp); return 0; }
          tok4 = strtok(NULL, " ");
          if(tok4==NULL) { pclose(fp); return 0; }
          if( strlen(tok4) < 17 ) { pclose(fp); return 0; }
          fprintf(stderr, "\nnext hop-> %s", tok4);
          tok4[2] = 0; tok4[5] = 0; tok4[8] = 0;
          tok4[11] = 0; tok4[14] = 0;
          dst_route[0] = strtol(&tok4[0],NULL,16);
          dst_route[1] = strtol(&tok4[3],NULL,16);
          dst_route[2] = strtol(&tok4[6],NULL,16);
          dst_route[3] = strtol(&tok4[9],NULL,16);
          dst_route[4] = strtol(&tok4[12],NULL,16);
          dst_route[5] = strtol(&tok4[15],NULL,16);
          pclose(fp);
          return 1;
        }
        memset(line,0x00,sizeof(line));
      }
      pclose(fp);
    } else {
      perror("is_batman_route:fopen/popen:");
    }
    return 0;
  }

clean:
if(fp!=NULL) fclose(fp);
  return 0;
}

////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////
int is_batman_route(const char*org_dest_mac_a, uint8_t *dst_route){

  // no_batman: ofdm0 is used as a plain L2 link (simulator, or a host doing
  // its own routing).  The next hop is the destination MAC itself.
  if(no_batman) {
    unsigned int m[6];
    if(sscanf(org_dest_mac_a, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6)
      return 0;
    for(int i = 0; i < 6; i++) dst_route[i] = (uint8_t)m[i];
    return 1;
  }

  if(route_cache_timer == NULL) {
    route_cache_timer = create_timer();
    timer_reset(route_cache_timer);
  }

  if(cache_valid && strncmp(org_dest_mac_a, cached_mac, 17) == 0 &&
     timer_elapsed_usec(route_cache_timer) < ROUTE_CACHE_TTL_USEC) {
    memcpy(dst_route, cached_route, 6);
    return 1;
  }

  int result = do_batman_route_lookup(org_dest_mac_a, dst_route);
  if(result) {
    strncpy(cached_mac, org_dest_mac_a, 17);
    cached_mac[17] = '\0';
    memcpy(cached_route, dst_route, 6);
    cache_valid = 1;
    timer_reset(route_cache_timer);
  } else {
    cache_valid = 0;
  }

  return result;
}
