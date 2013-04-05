/*
 * ipv6_reassembly.c
 *
 * Created on: 05/10/2012
 *
 * Support for IPv6 reassembly. Any modification done here should
 * be reflected on IPv4 reassembly.
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
#include <sys/types.h>
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <netinet/ip6.h>
#include <inttypes.h>
#include "ipv6_reassembly.h"
#include "reassembly.h"
#include "utils.h"
#if DPI_THREAD_SAFETY_ENABLED == 1
#include <ff/spin-lock.hpp>
#endif

#define DPI_DEBUG_FRAGMENTATION_v6 0

#define debug_print(fmt, ...)                    \
            do { if (DPI_DEBUG_FRAGMENTATION_v6) \
            fprintf(stderr, fmt, __VA_ARGS__); } while (0)

#define DPI_IP_FRAGMENTATION_MAX_DATAGRAM_SIZE 65535
#define DPI_IPv6_FRAGMENTATION_MINIMUM_MTU 1280


typedef struct dpi_ipv6_fragmentation_flow
               dpi_ipv6_fragmentation_flow_t;
typedef struct dpi_ipv6_fragmentation_source
               dpi_ipv6_fragmentation_source_t;


typedef struct dpi_ipv6_fragmentation_flow {
	/* Pointer to the unfragmentable part. */
	unsigned char *unfragmentable;
	u_int16_t unfragmentable_length;
	/*Total length of the final datagram (without unfragmentable part).*/
	u_int16_t len;
	/*
	 * NOTE: Differently from ipv6, in IPv6 the key is <id, src, dest>
	 * (so no next header value).
	 */
	u_int32_t id;
	struct in6_addr dstaddr;
	/* Linked list of received fragments. */
	dpi_reassembly_fragment_t *fragments;
	/*
	 * For a given source, pointer to the previous and next flows
	 * started from that source
	 */
	dpi_ipv6_fragmentation_flow_t *next;
	dpi_ipv6_fragmentation_flow_t *prev;
	dpi_reassembly_timer_t timer;
	dpi_ipv6_fragmentation_source_t *source;
}dpi_ipv6_fragmentation_flow_t;

typedef struct dpi_ipv6_fragmentation_source{
	dpi_ipv6_fragmentation_flow_t *flows;
	u_int32_t source_used_mem;
	struct in6_addr ipv6_srcaddr;
	u_int16_t row;
	dpi_ipv6_fragmentation_source_t* prev;
	dpi_ipv6_fragmentation_source_t* next;
}dpi_ipv6_fragmentation_source_t;

typedef struct dpi_ipv6_fragmentation_state{
	/**
	 *  Is an hash table containing associations between source IP
	 *  address and fragments generated by that address.
	 **/
	dpi_ipv6_fragmentation_source_t** table;
	u_int32_t total_used_mem;
	u_int16_t table_size;

	/** List of timers. **/
	dpi_reassembly_timer_t *timer_head, *timer_tail;

	/** Memory limits. **/
	u_int32_t per_source_memory_limit;
	u_int32_t total_memory_limit;

	/** Reassembly timeout. **/
	u_int8_t timeout;
#if DPI_THREAD_SAFETY_ENABLED == 1
	ff::CLHSpinLock lock;
#endif
}dpi_ipv6_fragmentation_state_t;


#ifndef DPI_DEBUG
static
#if DPI_USE_INLINING == 1
inline
#endif
#endif
void dpi_ipv6_fragmentation_delete_source(
		dpi_ipv6_fragmentation_state_t* state,
		dpi_ipv6_fragmentation_source_t* source);

/**
 * Enables the IPv6 defragmentation.
 * @param table_size  The size of the table used to store the fragments.
 * @return            A pointer to the IPv6 defragmentation handle.
 */
dpi_ipv6_fragmentation_state_t* dpi_reordering_enable_ipv6_fragmentation(
		u_int16_t table_size){
	dpi_ipv6_fragmentation_state_t* r=(dpi_ipv6_fragmentation_state_t*)
			calloc(1, sizeof(dpi_ipv6_fragmentation_state_t));
	if(r==NULL){
		return NULL;
	}
	r->table_size=table_size;
	r->table=(dpi_ipv6_fragmentation_source_t**)
			malloc(table_size * sizeof(dpi_ipv6_fragmentation_source_t*));
	if(r->table==NULL){
		free(r);
		return NULL;
	}
	u_int16_t i;
	for(i=0; i<table_size; i++){
		r->table[i]=NULL;
	}
	r->timer_head=NULL;
	r->timer_tail=NULL;
	r->per_source_memory_limit=
			DPI_IPv6_FRAGMENTATION_DEFAULT_PER_HOST_MEMORY_LIMIT;
	r->total_memory_limit=
			DPI_IPv6_FRAGMENTATION_DEFAULT_TOTAL_MEMORY_LIMIT;
	r->timeout=
			DPI_IPv6_FRAGMENTATION_DEFAULT_REASSEMBLY_TIMEOUT;
	r->total_used_mem=0;
#if DPI_THREAD_SAFETY_ENABLED == 1
	bzero(&(r->lock), sizeof(ff::CLHSpinLock));
	ff::init_unlocked(&(r->lock));
#endif
	return r;
}


/**
 * Sets the maximum amount of memory that can be used to store fragments
 * generated
 * by the same source.
 * @param frag_state A pointer to the IPv6 degragmentation handle.
 * @param per_host_memory_limit  The maximum amount of memory that can be
 *                               used to store fragments generated by the
 *                               same source.
 */
void dpi_reordering_ipv6_fragmentation_set_per_host_memory_limit(
		dpi_ipv6_fragmentation_state_t *frag_state,
		u_int32_t per_host_memory_limit){
	frag_state->per_source_memory_limit=per_host_memory_limit;
}

/**
 * Sets the maximum (global) amount of memory that can be used for
 * defragmentation purposes.
 * @param frag_state A pointer to the IPv6 defragmentation handle.
 * @param total_memory_limit   The global memory limit.
 */
void dpi_reordering_ipv6_fragmentation_set_total_memory_limit(
		dpi_ipv6_fragmentation_state_t *frag_state,
		u_int32_t total_memory_limit){
	frag_state->total_memory_limit=total_memory_limit;
}

/**
 * Sets the maximum amount of time (seconds) which can elapse before the
 * complete defragmentation of the datagram.
 * @param frag_state        A pointer to the IPv6 defragmentation handle.
 * @param timeout_seconds   The timeout (seconds).
 */
void dpi_reordering_ipv6_fragmentation_set_reassembly_timeout(
		dpi_ipv6_fragmentation_state_t *frag_state,
		u_int8_t timeout_seconds){
	frag_state->timeout=timeout_seconds;
}

/**
 * Disables the IPv6 fragmentation and deallocates the handle.
 * @param frag_state  A pointer to the IPv6 defragmentation handle.
 */
void dpi_reordering_disable_ipv6_fragmentation(
		dpi_ipv6_fragmentation_state_t* frag_state) {
	if(frag_state==NULL) return;
	dpi_ipv6_fragmentation_source_t *source,*tmp_source;
	if(frag_state->table){
		u_int16_t i;
		for(i=0; i<frag_state->table_size; i++){
			if(frag_state->table[i]){
				source=frag_state->table[i];
				while(source){
					tmp_source=source->next;
					dpi_ipv6_fragmentation_delete_source(frag_state,
							                             source);
					source=tmp_source;
				}
			}
		}
		free(frag_state->table);
	}
#if DPI_THREAD_SAFETY_ENABLED == 1
	frag_state->lock.~CLHSpinLock();
#endif
	free(frag_state);
}


#ifndef DPI_DEBUG
static
#if DPI_USE_INLINING == 1
inline
#endif
#endif
/**  Shift-Add-XOR hash. **/
u_int16_t dpi_ipv6_fragmentation_hash_function(
		dpi_ipv6_fragmentation_state_t* state, struct in6_addr addr){
	u_int16_t h=0;
	u_int8_t i;

	for(i=0; i<16; i++)
		h^=(h<<5)+(h>>2)+addr.s6_addr[i];

	return h%state->table_size;
}


#ifndef DPI_DEBUG
static
#endif
/**
 * Try to find the specific source. If it is not find, then creates it.
 * \param state The state of the defragmentation module.
 * \param addr The source address.
 * \returns A pointer to the source.
 */
dpi_ipv6_fragmentation_source_t*
       dpi_ipv6_fragmentation_find_or_create_source(
    		   dpi_ipv6_fragmentation_state_t* state,
    		   struct in6_addr addr){
	u_int16_t hash_index=dpi_ipv6_fragmentation_hash_function(state,
			                                                  addr);
	dpi_ipv6_fragmentation_source_t *source,*head;

	head=state->table[hash_index];

	for(source=head; source!=NULL; source=source->next){
		if(dpi_v6_addresses_equal(source->ipv6_srcaddr, addr)){
			return source;
		}
	}

	/** Not found, so create it. **/
	source=(dpi_ipv6_fragmentation_source_t*)
			malloc(sizeof(dpi_ipv6_fragmentation_source_t));
	if(unlikely(source==NULL)){
		return NULL;
	}
	source->row=hash_index;
	source->flows=NULL;

	source->ipv6_srcaddr=addr;
	source->source_used_mem=sizeof(dpi_ipv6_fragmentation_source_t);
	state->total_used_mem+=sizeof(dpi_ipv6_fragmentation_source_t);


	/** Insertion at the beginning of the list. **/
	source->prev=NULL;
	source->next=head;
	if(head)
		head->prev=source;
	state->table[hash_index]=source;

	return source;
}

#ifndef DPI_DEBUG
static
#endif
void dpi_ipv6_fragmentation_delete_flow(
		dpi_ipv6_fragmentation_state_t* state,
		dpi_ipv6_fragmentation_flow_t* flow){
	dpi_reassembly_fragment_t *frag,*temp_frag;

	dpi_ipv6_fragmentation_source_t* source=flow->source;

	source->source_used_mem-=sizeof(dpi_ipv6_fragmentation_flow_t);
	state->total_used_mem-=sizeof(dpi_ipv6_fragmentation_flow_t);

	/* Stop the timer and delete it. */
	dpi_reassembly_delete_timer(&(state->timer_head),
			                    &(state->timer_tail), &(flow->timer));

	/* Release all fragment data. */
	frag = flow->fragments;
	while(frag){
		temp_frag=frag->next;
		source->source_used_mem-=(frag->end-frag->offset);
		state->total_used_mem-=(frag->end-frag->offset);

		free(frag->ptr);
		free(frag);
		frag=temp_frag;
	}

	/** Delete the IP header. **/
	if(flow->unfragmentable){
		source->source_used_mem-=flow->unfragmentable_length;
		state->total_used_mem-=flow->unfragmentable_length;
		free(flow->unfragmentable);
	}

	/*
	 * Remove the flow from the list of the flows. If no more flows for
	 * this source, then delete the source.
	 */
	if(flow->prev==NULL){
		source->flows=flow->next;
		if(source->flows!=NULL)
			source->flows->prev=NULL;
	}else{
		flow->prev->next=flow->next;
		if(flow->next)
			flow->next->prev=flow->prev;
	}
	free(flow);
}


/*
 * Find the flow. If it is not found creates it.
 * \returns A pointer to the flow.
 */
#ifndef DPI_DEBUG
static
#endif
dpi_ipv6_fragmentation_flow_t* dpi_ipv6_fragmentation_find_or_create_flow(
		dpi_ipv6_fragmentation_state_t* state,
		dpi_ipv6_fragmentation_source_t* source,
		u_int32_t id, struct in6_addr dstaddr,
		u_int32_t current_time){
	dpi_ipv6_fragmentation_flow_t* flow;
	for(flow=source->flows; flow!=NULL; flow=flow->next){
		/**
		 * The source is matched for sure because all the flows
		 * will have the same source.
		 **/
		if(id==flow->id && dpi_v6_addresses_equal(dstaddr,
				                                  flow->dstaddr)){
			return flow;
		}
	}

	/** Not found, create a new flow. **/
	flow=(dpi_ipv6_fragmentation_flow_t*)
			malloc(sizeof(dpi_ipv6_fragmentation_flow_t));
	if(unlikely(flow==NULL)){
		return NULL;
	}

	source->source_used_mem+=sizeof(dpi_ipv6_fragmentation_flow_t);
	state->total_used_mem+=sizeof(dpi_ipv6_fragmentation_flow_t);


	flow->fragments=NULL;
	flow->source=source;
	flow->len=0;
	/* Add this entry to the queue of flows. */
	flow->prev=NULL;
	flow->next=source->flows;
	if(flow->next)
		flow->next->prev=flow;
	source->flows=flow;
	/* Set the timer. */
	flow->timer.expiration_time=current_time+state->timeout;
	flow->timer.data = flow;
	dpi_reassembly_add_timer(&(state->timer_head), &(state->timer_tail),
			                 &(flow->timer));
	/* Fragments will be added later. */
	flow->fragments=NULL;
	flow->unfragmentable=NULL;
	flow->id=id;
	flow->dstaddr=dstaddr;
	return flow;
}


/**
 * @return A pointer to the recompacted datagram or NULL if an error
 * occurred.
 */
#ifndef DPI_DEBUG
static
#endif
unsigned char* dpi_ipv6_fragmentation_build_complete_datagram(
		dpi_ipv6_fragmentation_state_t* state,
		dpi_ipv6_fragmentation_flow_t* flow){
	unsigned char *pkt_beginning, *pkt_data;
	struct ip6_hdr *iph;
	u_int16_t len;
	int32_t count;

	/* Allocate a new buffer for the datagram. */
	len=flow->len;

	dpi_ipv6_fragmentation_source_t* source=flow->source;

	u_int32_t tot_len=flow->unfragmentable_length+len;

	if(unlikely(tot_len>DPI_IP_FRAGMENTATION_MAX_DATAGRAM_SIZE)){
		dpi_ipv6_fragmentation_delete_flow(state, flow);
		if(source->flows==NULL)
			dpi_ipv6_fragmentation_delete_source(state,source);
		return NULL;
	}


	if(unlikely((pkt_beginning=(unsigned char*)
			malloc(flow->unfragmentable_length+len))==NULL)){
		dpi_ipv6_fragmentation_delete_flow(state, flow);
		if(source->flows==NULL)
			dpi_ipv6_fragmentation_delete_source(state,source);
		return NULL;
	}

	memcpy(pkt_beginning, (flow->unfragmentable),
		   flow->unfragmentable_length);
	pkt_data=pkt_beginning+flow->unfragmentable_length;

	count=dpi_reassembly_ip_compact_fragments(flow->fragments,
			                                  &pkt_data, len);

	/**
	 * Misbehaving packet, real size is different from that obtained
	 * from the last fragment.
	 **/
	if(count==-1){
		free(pkt_beginning);
		return NULL;
	}

	/** Put the correct informations in the IP header. **/
	iph=(struct ip6_hdr *) pkt_beginning;
	iph->ip6_ctlun.ip6_un1.ip6_un1_plen=
			htons(count+flow->unfragmentable_length-
				  sizeof(struct ip6_hdr));

	/** We recompacted the flow (datagram), so now we can delete it. **/
	dpi_ipv6_fragmentation_delete_flow(state, flow);
	if(source->flows==NULL)
		dpi_ipv6_fragmentation_delete_source(state,source);

	return pkt_beginning;
}


#ifndef DPI_DEBUG
static
#if DPI_USE_INLINING == 1
inline
#endif
#endif
void dpi_ipv6_fragmentation_delete_source(
		dpi_ipv6_fragmentation_state_t* state,
		dpi_ipv6_fragmentation_source_t* source) {
	u_int16_t row=source->row;

	/** Delete all the flows belonging to this source. **/
	dpi_ipv6_fragmentation_flow_t* flow=source->flows,*temp_flow;
	while(flow){
		temp_flow=flow->next;
		dpi_ipv6_fragmentation_delete_flow(state, flow);
		flow=temp_flow;
	}

	/** Delete this source from the list. **/
	if (source->prev)
		source->prev->next=source->next;
	else
		state->table[row]=source->next;

	if(source->next)
		source->next->prev=source->prev;

	free(source);
	state->total_used_mem-=sizeof(dpi_ipv6_fragmentation_source_t);
}



/**
 * Reassemble the IP datagram if it is fragmented.
 * @param state The state for fragmentation support.
 * @param unfragmentable_start A pointer to the beginning of
 * unfragmentable part. We suppose that unfragmentable and
 * fragmentable data are in the same buffer.
 * @param unfragmentable_size The size of the unfragmentable part.
 * In principle this could obtained by difference (fragmentable_start-
 * unfragmentable_start). By the way this additional level of flexibility
 * allows to store only a part of the unfragmentable header (e.g. only the
 * IPv6 header without additional headers).
 * @param fragmentable_start A pointer to the beginning of the
 * fragmentable part.
 * @param fragmentable_size Size of the fragmentable part.
 * @param offset The data offset specified in the IPv6 fragmentation
 * header.
 * @param more_fragments 1 if the MF flag is set, 0 otherwise.
 * @param identification The identification of the datagram.
 * @param next_header The header which follows the fragmentation
 * header.
 * @param current_time The current time, in seconds.
 * @param tid The thread identifier.
 * @return Returns NULL if the datagram is a fragment but doesn't fill
 * an hole. In this case, the content of the datagram has been copied,
 * so if the user wants, he can release the resources used to store the
 * received packet.
 * Returns a pointer to the recomposed datagram if the datagram is the
 * last fragment of a bigger datagram. This pointer will be different
 * from data. The user should free() this pointer when it is no more
 * needed.
 */
unsigned char* dpi_reordering_manage_ipv6_fragment(
		dpi_ipv6_fragmentation_state_t *state,
		const unsigned char* unfragmentable_start,
		u_int16_t unfragmentable_size,
		const unsigned char* fragmentable_start,
		u_int16_t fragmentable_size,
		u_int16_t offset, u_int8_t more_fragments,
		u_int32_t identification, u_int8_t next_header,
		u_int32_t current_time, int tid){
	dpi_ipv6_fragmentation_source_t* source,*tmpsource;
	dpi_ipv6_fragmentation_flow_t* flow;

	struct ip6_hdr* ip6=(struct ip6_hdr*) unfragmentable_start;
	/**
	 * Host are required to do not fragment datagrams with a total size
	 * up to 576 byte. If we received a fragment with a size <576 it is
	 * maybe a forged fragment used to make an attack. We do this check
	 * only in non-debug situations because many of the test used to
	 * validate the ip reassembly contains small packets.
	 */
#ifndef DPI_DEBUG_FRAGMENTATION_v6
	if(unlikely(fragmentable_start+fragmentable_size-unfragmentable_start<
			    DPI_IPv6_FRAGMENTATION_MINIMUM_MTU)){
		return NULL;
	}
#endif

	/** (end-1) is the last byte of the fragment. **/
	u_int32_t end=offset+fragmentable_size;

	/* Attempt to construct an oversize packet. */
	if(unlikely(end>DPI_IP_FRAGMENTATION_MAX_DATAGRAM_SIZE)){
		debug_print("%s\n","Attempt to build an oversized packet");
		return NULL;
	}

#if DPI_THREAD_SAFETY_ENABLED == 1
	ff::spin_lock(&(state->lock), tid);
#endif
	source=dpi_ipv6_fragmentation_find_or_create_source(
			 state, ip6->ip6_src);

	if(unlikely(source==NULL)){
		debug_print("%s\n","ERROR: Impossible to create the source. "
				    "Memory exhausted.");
#if DPI_THREAD_SAFETY_ENABLED == 1
		ff::spin_unlock(&(state->lock), tid);
#endif
		return NULL;
	}
	debug_print("%s\n","Source found or created.");

	debug_print("Total memory occupied: %d\n", state->total_used_mem);
	debug_print("Source memory occupied: %d\n", state->total_used_mem);


	/**
	 * If I exceeded the source limit, then delete flows from that
	 * source.
	 **/
	while(source->flows && (source->source_used_mem)>
	      state->per_source_memory_limit){
		debug_print("%s\n","Source limit exceeded, cleaning...");
		dpi_ipv6_fragmentation_delete_flow(state, source->flows);
		if(source->flows==NULL){
			dpi_ipv6_fragmentation_delete_source(state, source);
#if DPI_THREAD_SAFETY_ENABLED == 1
			ff::spin_unlock(&(state->lock), tid);
#endif
			return NULL;
		}
	}

	/**
	 * Control on global memory limit for ip fragmentation.
	 * The timer are sorted for the one which will expire sooner to the
	 * last that will expire. The loop stops when there are no more
	 * expired timers. dpi_ipv4_fragmentation_delete_flow(..) update
	 * the timer timer_head after deleting the timer_head if it is
	 * expired.
	 **/
	while((state->timer_head) &&
		  ((state->timer_head->expiration_time<current_time) ||
		  (state->total_used_mem>=state->total_memory_limit))){
		tmpsource=((dpi_ipv6_fragmentation_flow_t*)
				     state->timer_head->data)->source;
		dpi_ipv6_fragmentation_delete_flow(state,
				                          (dpi_ipv6_fragmentation_flow_t*)
				                          state->timer_head->data);
		if(source->flows==NULL){
			dpi_ipv6_fragmentation_delete_source(state,tmpsource);
#if DPI_THREAD_SAFETY_ENABLED == 1
			ff::spin_unlock(&(state->lock), tid);
#endif
			return NULL;
		}
	}


	/* Find the flow. */
	flow=dpi_ipv6_fragmentation_find_or_create_flow(state, source,
			                                        identification,
			                                        ip6->ip6_dst,
			                                        current_time);

	if(unlikely(flow==NULL)){
		debug_print("%s\n","ERROR: Impossible to create the flow.");
#if DPI_THREAD_SAFETY_ENABLED == 1
		ff::spin_unlock(&(state->lock), tid);
#endif
		return NULL;
	}

	debug_print("%s\n","Flow found or created.");

	/**
	 * If is a malformed fragment which starts after the end of
	 * the entire datagram.
	 **/
	if(unlikely(flow->len!=0 && offset>flow->len)){
#if DPI_THREAD_SAFETY_ENABLED == 1
		ff::spin_unlock(&(state->lock), tid);
#endif
		return NULL;
	}

	/*
	 * The unfragmentable part is the same for all the fragments. So,
	 * differently from IPv4, we don't have to check that the offset is
	 * zero to store it but only that it is not already present.
	 */
	if(flow->unfragmentable==NULL){
		flow->unfragmentable=(unsigned char*)
				malloc(unfragmentable_size*sizeof(unsigned char));
		if(unlikely(flow->unfragmentable==NULL)){
			dpi_ipv6_fragmentation_delete_flow(state, flow);
#if DPI_THREAD_SAFETY_ENABLED == 1
			ff::spin_unlock(&(state->lock), tid);
#endif
			return NULL;
		}
		flow->unfragmentable_length=unfragmentable_size;
		state->total_used_mem+=unfragmentable_size;
		source->source_used_mem+=unfragmentable_size;
		memcpy(flow->unfragmentable, unfragmentable_start,
			   unfragmentable_size);
		((struct ip6_hdr*) flow->unfragmentable)->ip6_ctlun.
				           ip6_un1.ip6_un1_nxt=next_header;
	}

	debug_print("More fragments: %d\n", more_fragments);
	/**
	 * If is the final fragment, then we know the exact data_length
	 * of the original datagram.
	 **/
	if(!more_fragments){
		debug_print("%s\n","Last fragment received.");
		/**
		 * If the data with MF flag=0 was already received then this
		 * fragment is useless.
		 **/
		if(flow->len!=0){
#if DPI_THREAD_SAFETY_ENABLED == 1
			ff::spin_unlock(&(state->lock), tid);
#endif
			return NULL;
		}
		flow->len=end;
	}

	u_int32_t bytes_removed;
	u_int32_t bytes_inserted;
	dpi_reassembly_insert_fragment(&(flow->fragments), fragmentable_start,
			                       offset, end, &bytes_removed,
			                       &bytes_inserted);

	state->total_used_mem+=bytes_inserted;
	state->total_used_mem-=bytes_removed;

	source->source_used_mem+=bytes_inserted;
	source->source_used_mem-=bytes_removed;

	debug_print("%s\n","Fragment inserted.");

	/**
	 *  Check if with the new fragment that we inserted, the original
	 *  datagram is now complete. (Only possible if we received the
	 *  fragment with MF flag=0 (so the len is set) and if we have a
	 *  train of contiguous fragments).
	 **/
	if(flow->len!=0 &&
	   dpi_reassembly_ip_check_train_of_contiguous_fragments(
			    flow->fragments)){
		unsigned char* r;
		debug_print("%s\n","Last fragment already received and train of "
				    "contiguous fragments present, returing the recompacted "
				    "datagram.");
		r=dpi_ipv6_fragmentation_build_complete_datagram(state,flow);
#if DPI_THREAD_SAFETY_ENABLED == 1
		ff::spin_unlock(&(state->lock), tid);
#endif
		return r;
	}
#if DPI_THREAD_SAFETY_ENABLED == 1
	ff::spin_unlock(&(state->lock), tid);
#endif
	return NULL;

}

