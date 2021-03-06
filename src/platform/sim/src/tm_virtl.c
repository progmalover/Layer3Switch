#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>      
#include <sys/stat.h>
#include <sys/socket.h>
#include <netpacket/packet.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <mqueue.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <getopt.h>
#include <netdb.h>
#include <asm/types.h>
#include "common_types.h"
#include "list.h"
#include "types_sim.h"

#define ARPHRD_ETHER    1

#define VERSION "0.1"

#define TM_VIRT_PORT 9000
#define TM_INST_PORT  TM_VIRT_PORT  + tm_inst

void vlink_processing_task (void *unused);
void tx_pkt (void *buf,  int len);
extern void dump_task_info (void);
int open_message_queue (uint8_t inst);
int parse_cmdline (int argc, char *argv[]);
int spawn_pkt_processing_task (void);
void * packet_processing_task (void *unused);
int create_raw_sock_for_pkt_capture (void);
int rcv_pkt (void *buf);
void write_string (const char *str);
void process_pkt (void  *pkt, int len, uint16_t port);
void tx_pkt_for_capture (uint8_t *buf, int len) ;
int ifname_info (char *ifname);
int create_communication_channel (void);


#pragma pack(push)  /* push current alignment to stack */
#pragma pack(1)     /* set alignment to 1 byte boundary */
typedef struct inst {
	uint8_t inst;
	uint8_t port;
}inst_t;
#pragma pack(pop)   /* restore original alignment from stack */

static int32_t  sockid = 0;
static int32_t  sockid_pkt = 0;
static uint32_t tm_virt_ip = 0;
static uint32_t tm_ifindex = 0;
static uint32_t tm_inst = 0;
static char     tm_ifname[IFNAMSIZ] = "lo";

void * packet_processing_task (void *unused)
{
	int len = 0;

	int retry = 0;

	char *pkt_buf = NULL;

	inst_t *inst = NULL;

	char buf[MAX_MTU];

	memset (buf, 0, sizeof(MAX_MTU));

	while (1) {


		len = rcv_pkt (buf);
		if (len < 0) {
			continue;
		}
		inst = (inst_t *)buf;	

		if (inst->inst != tm_inst) {
			printf ("Invalid Inst\n");
			continue;
		}

		pkt_buf = tm_malloc (len - sizeof(inst_t));

		if (!pkt_buf) {
			printf ("Out of memory\n");
			sleep (1); /*Sleep for some time*/
			if (++retry == 3) {
				write_string ("Out of Memory!\n");
				exit (1);
			}
			continue;
		}

		memcpy (pkt_buf, &buf[2], len - sizeof(inst_t));

		process_pkt (pkt_buf, len - sizeof(inst_t), inst->port);

		++gsw_info.cpu_pkt_count;
	}

	return 0;
}

void send_packet (void *buf, uint16_t port, int len)
{
	char pkt[MAX_MTU];

	tx_pkt_for_capture (buf, len);

	memset (pkt, 0, MAX_MTU);

	pkt[0] = tm_inst;
	pkt[1] = (uint8_t)port;

	memcpy (&pkt[2], buf, len);

	tx_pkt (pkt, len + sizeof(inst_t));
}

void tx_pkt (void *buf, int len) 
{
	struct sockaddr_in dest_addr;

	memset (&dest_addr, 0, sizeof(dest_addr));

	dest_addr.sin_family = AF_INET;
	dest_addr.sin_port = htons(TM_VIRT_PORT);
	dest_addr.sin_addr.s_addr = htonl (tm_virt_ip);

	if (sendto (sockid, buf, len, 0,(struct sockaddr *)&dest_addr, 
				sizeof(dest_addr)) < 0) {
		perror ("-ERR- SENDTO: ");
	}
	return;
}

void tx_pkt_for_capture (uint8_t *buf, int len) 
{
	struct sockaddr_ll socket_address;

	memset (&socket_address, 0, sizeof(socket_address));

	socket_address.sll_family   = PF_PACKET;	
	socket_address.sll_protocol = htons(ETH_P_ALL);	
	socket_address.sll_ifindex  = tm_ifindex;
	socket_address.sll_pkttype  = PACKET_HOST;
	socket_address.sll_halen    = ETH_ALEN;		

        if (sendto (sockid_pkt, buf, len, 0,(struct sockaddr *)&socket_address,
                                sizeof(socket_address)) < 0) {
		;
        }
	return;
}

int ifname_info (char *ifname)
{
	struct ifreq	ifr;
	int		fd	= socket(AF_INET, SOCK_DGRAM, 0);

	if (fd < 0) 	
		return (-1);

	strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));

	if (ioctl(fd, SIOCGIFADDR, (char *)&ifr) == 0) {
		struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
		tm_virt_ip = ntohl(sin->sin_addr.s_addr);
	}

	if (ioctl(fd, SIOCGIFINDEX, (char *)&ifr) == 0)
		tm_ifindex = ifr.ifr_ifindex;

	close(fd);

	if (!tm_virt_ip)
		return -1;

	return 0;
}


int spawn_pkt_processing_task (void)
{
	tmtaskid_t tid = 0;
	return task_create ("PKRX", 98, 3, 32000, packet_processing_task, NULL, NULL, &tid);
}


int create_communication_channel (void)
{
	struct sockaddr_in si_me;

	memset((char *) &si_me, 0, sizeof(si_me));

	if ((sockid =socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
		perror ("SOCKET");
		return -1;
	}

	si_me.sin_family = AF_INET;
	si_me.sin_port = htons(TM_INST_PORT);
	si_me.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(sockid, (struct sockaddr *)&si_me, sizeof(si_me)) < 0) {
		perror ("BIND");
		return -1;
	}
	return  0;
}

int create_raw_sock_for_pkt_capture (void)
{
	struct sockaddr_in si_me;

	memset((char *) &si_me, 0, sizeof(si_me));

	if ((sockid_pkt =socket(AF_PACKET, SOCK_RAW, htons (ETH_P_ALL))) < 0) {
		perror ("SOCKET");
		return -1;
	}

	return  0;
}

int rcv_pkt (void *buf)
{
	int len = 0;
	struct sockaddr_in si_other;
	socklen_t slen=sizeof(si_other);

	len = recvfrom (sockid, buf, ETH_FRAME_LEN, 0, (struct sockaddr *) &si_other, &slen);
#ifdef PKT_DBG
	printf("Received packet from %s:%d\nData: %s\n\n", 
        	inet_ntoa(si_other.sin_addr), ntohs(si_other.sin_port), buf);
#endif

	return len;
}

int parse_cmdline (int argc, char *argv[])
{
	tm_inst = atoi(argv[1]);

	ifname_info (tm_ifname);

	create_communication_channel ();	

	create_raw_sock_for_pkt_capture ();

	return 0;
}
