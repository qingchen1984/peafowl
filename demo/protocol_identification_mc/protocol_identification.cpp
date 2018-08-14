/*
 * demo_identification.c
 *
 * Given a .pcap file, it identifies the protocol of all the packets contained in it.
 *
 * Created on: 12/11/2012
 *
 * =========================================================================
 *  Copyright (C) 2012-2013, Daniele De Sensi (d.desensi.software@gmail.com)
 *
 *  This file is part of Peafowl.
 *
 *  Peafowl is free software: you can redistribute it and/or
 *  modify it under the terms of the Lesser GNU General Public
 *  License as published by the Free Software Foundation, either
 *  version 3 of the License, or (at your option) any later version.

 *  Peafowl is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  Lesser GNU General Public License for more details.
 *
 *  You should have received a copy of the Lesser GNU General Public
 *  License along with Peafowl.
 *  If not, see <http://www.gnu.org/licenses/>.
 *
 * =========================================================================
 */

#include <peafowl/mc_api.h>
#include <pcap.h>
#include <net/ethernet.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <assert.h>

#define SIZE_IPv4_FLOW_TABLE 32767
#define SIZE_IPv6_FLOW_TABLE 32767
#define MAX_IPv4_ACTIVE_FLOWS 500000
#define MAX_IPv6_ACTIVE_FLOWS 500000

#define AVAILABLE_PROCESSORS 8

int datalink_type=0;

u_int32_t protocols[DPI_NUM_PROTOCOLS];
u_int32_t unknown=0;

typedef struct{
	pcap_t* handle;
	size_t ip_offset;
}reading_cb_data;

/**
 * This function will be called by the library (active mode only) to read
 * a packet from the network.
 * @param callback_data   A pointer to user specified data (e.g.
 *                        network socket).
 * @return                The packet read. If the pkt field is NULL, then
 *                        there are no more data to read and the library
 *                        will terminate. The user must never try to
 *                        modify the state after that he returned
 *                        pkt=NULL, otherwise the behaviour is not
 *                        defined.
 */
mc_dpi_packet_reading_result_t reading_cb(void* callback_data){
	pcap_t *handle = ((reading_cb_data*) callback_data)->handle;
	size_t ip_offset = ((reading_cb_data*) callback_data)->ip_offset;
	struct pcap_pkthdr header;
	bool goodpacket = false;

	mc_dpi_packet_reading_result_t res;
	do{
			const u_char* packet = pcap_next(handle, &header);
		res.pkt = NULL;

		size_t len = 0;
		uint virtual_offset = 0;
		if(packet){
			if(datalink_type == DLT_EN10MB){
	            if(header.caplen < ip_offset){
	                continue;
	            }
	            uint16_t ether_type = ((struct ether_header*) packet)->ether_type;
	            if(ether_type == htons(0x8100)){ // VLAN
	                virtual_offset = 4;
	            }
	            if(ether_type != htons(ETHERTYPE_IP) &&
	               ether_type != htons(ETHERTYPE_IPV6)){
	                continue;
	            }
	        }
	        len = header.caplen - ip_offset - virtual_offset;
			u_char* packetCopy = (u_char*) malloc(sizeof(u_char)*len);
			memcpy(packetCopy, packet + ip_offset + virtual_offset, sizeof(u_char)*len);
			res.pkt = packetCopy;
			res.user_pointer = packetCopy;
		}
		res.length = len;
		res.current_time = time(NULL);
		goodpacket = true;
	}while(!goodpacket);
	return res;
}



/**
 * This function will be called by the library (active mode only) to
 * process the result of the protocol identification.
 * @param processing_result   A pointer to the result of the library
 *                            processing.
 * @param callback_data       A pointer to user specified data (e.g.
 *                            network socket).
 */
void processing_cb(mc_dpi_processing_result_t* processing_result, void* callback_data){
	dpi_identification_result_t r = processing_result->result;
    if(r.protocol.l4prot == IPPROTO_TCP ||
       r.protocol.l4prot == IPPROTO_UDP){
        if(r.protocol.l7prot < DPI_NUM_PROTOCOLS){
            ++protocols[r.protocol.l7prot];
        }else{
            ++unknown;
        }
    }else{
        ++unknown;
    }
    free(processing_result->user_pointer);
}


int main(int argc, char** argv){
	if(argc!=2){
		fprintf(stderr, "Usage: %s pcap_file\n", argv[0]);
		return -1;
	}
	char* pcap_filename=argv[1];
	char errbuf[PCAP_ERRBUF_SIZE];

	mc_dpi_parallelism_details_t par;
	memset(&par, 0, sizeof(par));
	par.available_processors = AVAILABLE_PROCESSORS;
	mc_dpi_library_state_t* state = mc_dpi_init_stateful(SIZE_IPv4_FLOW_TABLE, SIZE_IPv6_FLOW_TABLE, MAX_IPv4_ACTIVE_FLOWS, MAX_IPv6_ACTIVE_FLOWS, par);
	pcap_t *handle=pcap_open_offline(pcap_filename, errbuf);

	if(handle==NULL){
		fprintf(stderr, "Couldn't open device %s: %s\n", pcap_filename, errbuf);
		return (2);
	}

	datalink_type=pcap_datalink(handle);
	uint ip_offset=0;
	if(datalink_type==DLT_EN10MB){
		printf("Datalink type: Ethernet\n");
		ip_offset=sizeof(struct ether_header);
	}else if(datalink_type==DLT_RAW){
		printf("Datalink type: RAW\n");
		ip_offset=0;
	}else if(datalink_type==DLT_LINUX_SLL){
		printf("Datalink type: Linux Cooked\n");
		ip_offset=16;
	}else{
		fprintf(stderr, "Datalink type not supported\n");
		exit(-1);
	}

    memset(protocols, 0, sizeof(protocols));
	/** Set callback to read packets from the network and to process the result of the identification (and maybe forward the packet). **/
	reading_cb_data cbd;
	cbd.handle = handle;
	cbd.ip_offset = ip_offset;
    mc_dpi_set_core_callbacks(state, reading_cb, processing_cb, (void*) &cbd);
	mc_dpi_run(state);

	mc_dpi_wait_end(state);
	mc_dpi_terminate(state);


	if (unknown > 0) printf("Unknown packets: %" PRIu32 "\n", unknown);
    for(size_t i = 0; i < DPI_NUM_PROTOCOLS; i++){
        if (protocols[i] > 0) printf("%s packets: %" PRIu32 "\n", dpi_get_protocol_string(i), protocols[i]);
    }
	return 0;
}


