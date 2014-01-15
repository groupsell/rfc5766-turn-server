/*
 * Copyright (C) 2011, 2012, 2013 Citrix Systems
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "ns_turn_allocation.h"

/////////////// Permission forward declarations /////////////////

static void init_turn_permission_hashtable(turn_permission_hashtable *map);
static void free_turn_permission_hashtable(turn_permission_hashtable *map);
static turn_permission_info* get_from_turn_permission_hashtable(turn_permission_hashtable *map, const ioa_addr *addr);

/////////////// ALLOCATION //////////////////////////////////////

void init_allocation(void *owner, allocation* a, ur_map *tcp_connections) {
  if(a) {
    ns_bzero(a,sizeof(allocation));
    a->owner = owner;
    lm_map_init(&(a->chn_to_ch_info));
    a->tcp_connections = tcp_connections;
    init_turn_permission_hashtable(&(a->addr_to_perm));
  }
}

void clear_allocation(allocation *a)
{
	if (a) {

		if(a->is_valid)
			turn_report_allocation_delete(a);

		while(a->tcl.next) {
			tcp_connection *tc = (tcp_connection*)(a->tcl.next);
			delete_tcp_connection(tc);
		}

		clear_ioa_socket_session_if(a->relay_session.s, a->owner);
		clear_ts_ur_session_data(&(a->relay_session));

		IOA_EVENT_DEL(a->lifetime_ev);

		/* The order is important here: */
		free_turn_permission_hashtable(&(a->addr_to_perm));
		lm_map_clean(&(a->chn_to_ch_info));

		a->is_valid=0;
	}
}

ts_ur_session *get_relay_session(allocation *a)
{
	return &(a->relay_session);
}

ioa_socket_handle get_relay_socket(allocation *a)
{
	return a->relay_session.s;
}

void set_allocation_lifetime_ev(allocation *a, turn_time_t exp_time, ioa_timer_handle ev)
{
	if (a) {
		IOA_EVENT_DEL(a->lifetime_ev);
		a->expiration_time = exp_time;
		a->lifetime_ev = ev;
	}
}

int is_allocation_valid(const allocation* a) {
  if(a) return a->is_valid;
  else return 0;
}

void set_allocation_valid(allocation* a, int value) {
  if(a) a->is_valid=value;
}

turn_permission_info* allocation_get_permission(allocation* a, const ioa_addr *addr) {
  if(a) {
    return get_from_turn_permission_hashtable(&(a->addr_to_perm), addr);
  }
  return NULL;
}

///////////////////////////// TURN_PERMISSION /////////////////////////////////

static int delete_channel_info_from_allocation_map(ur_map_key_type key, ur_map_value_type value);

void turn_permission_clean(turn_permission_info* tinfo)
{
	if (tinfo && tinfo->allocated) {

		if(!(tinfo->lifetime_ev)) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "!!! %s: strange (1) permission to be cleaned\n",__FUNCTION__);
		}

		IOA_EVENT_DEL(tinfo->lifetime_ev);
		lm_map_foreach(&(tinfo->chns), (foreachcb_type) delete_channel_info_from_allocation_map);
		lm_map_clean(&(tinfo->chns));
		ns_bzero(tinfo,sizeof(turn_permission_info));
	}
}

static void init_turn_permission_hashtable(turn_permission_hashtable *map)
{
	if (map)
		ns_bzero(map,sizeof(turn_permission_hashtable));
}

static void free_turn_permission_hashtable(turn_permission_hashtable *map)
{
	if(map) {

		size_t i;
		for(i=0;i<TURN_PERMISSION_HASHTABLE_SIZE;++i) {

			turn_permission_array *parray = &(map->table[i]);

			{
				size_t j;
				for(j=0;j<TURN_PERMISSION_ARRAY_SIZE;++j) {
					turn_permission_slot *slot = &(parray->main_slots[j]);
					if(slot->info.allocated) {
						turn_permission_clean(&(slot->info));
					}
				}
			}

			if(parray->extra_slots) {
				size_t j;
				for(j=0;j<parray->extra_sz;++j) {
					turn_permission_slot *slot = parray->extra_slots[j];
					if(slot) {
						if(slot->info.allocated) {
							turn_permission_clean(&(slot->info));
						}
						turn_free(slot,sizeof(turn_permission_slot));
					}
				}
				turn_free(parray->extra_slots, parray->extra_sz * sizeof(turn_permission_slot*));
				parray->extra_slots = NULL;
			}
			parray->extra_sz = 0;
		}
	}
}

static turn_permission_info* get_from_turn_permission_hashtable(turn_permission_hashtable *map, const ioa_addr *addr)
{
	if (!addr || !map)
		return NULL;

	u32bits index = addr_hash_no_port(addr) & (TURN_PERMISSION_HASHTABLE_SIZE-1);
	turn_permission_array *parray = &(map->table[index]);

	{
		size_t i;
		for (i = 0; i < TURN_PERMISSION_ARRAY_SIZE; ++i) {
			turn_permission_slot *slot = &(parray->main_slots[i]);
			if (slot->info.allocated && addr_eq_no_port(&(slot->info.addr), addr)) {
				return &(slot->info);
			}
		}
	}

	if(parray->extra_slots) {

		size_t i;
		size_t sz = parray->extra_sz;
		for (i = 0; i < sz; ++i) {
			turn_permission_slot *slot = parray->extra_slots[i];
			if (slot->info.allocated && addr_eq_no_port(&(slot->info.addr), addr)) {
				return &(slot->info);
			}
		}
	}

	return NULL;
}

static void ch_info_clean(ur_map_value_type value) {
  if(value) {
    ch_info* c = (ch_info*)value;
    IOA_EVENT_DEL(c->lifetime_ev);
    ns_bzero(c,sizeof(ch_info));
  }
}

static int delete_channel_info_from_allocation_map(ur_map_key_type key, ur_map_value_type value)
{
	UNUSED_ARG(key);

	if(value) {
		int found = 0;
		ch_info* chn = (ch_info*)value;
		turn_permission_info* tinfo = (turn_permission_info*)chn->owner;
		if(tinfo) {
			allocation* a = (allocation*)tinfo->owner;
			if(a) {
				found = lm_map_del(&(a->chn_to_ch_info), chn->chnum, ch_info_clean);
				if(!found) {
					TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "!!! %s: strange (1) channel to be cleaned: not found\n",__FUNCTION__);
				}
			} else {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "!!! %s: strange (2) channel to be cleaned: allocation is empty\n",__FUNCTION__);
			}
		} else {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "!!! %s: strange (3) channel to be cleaned: permission is empty\n",__FUNCTION__);
		}

		if(!found) {
			ch_info_clean(value);
		}

		turn_free(chn,sizeof(ch_info));
	}

	return 0;
}

void turn_channel_delete(ch_info* chn)
{
	if(chn) {
		int port = addr_get_port(&(chn->peer_addr));
		if(port<1) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "!!! %s: strange (1) channel to be cleaned: port is empty\n",__FUNCTION__);
		}
		{
			turn_permission_info* tinfo = (turn_permission_info*)chn->owner;
			if(tinfo) {
				lm_map_del(&(tinfo->chns), (ur_map_key_type)port,NULL);
			} else {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "!!! %s: strange (2) channel to be cleaned: permission is empty\n",__FUNCTION__);
			}
		}
		delete_channel_info_from_allocation_map((ur_map_key_type)port,(ur_map_value_type)chn);
	}
}

ch_info* allocation_get_new_ch_info(allocation* a, u16bits chnum, ioa_addr* peer_addr)
{

	turn_permission_info* tinfo = get_from_turn_permission_hashtable(&(a->addr_to_perm), peer_addr);

	if (!tinfo)
		tinfo = allocation_add_permission(a, peer_addr);

	ch_info* chn = (ch_info*)turn_malloc(sizeof(ch_info));

	ns_bzero(chn,sizeof(ch_info));

	chn->chnum = chnum;
	chn->port = addr_get_port(peer_addr);
	addr_cpy(&(chn->peer_addr), peer_addr);
	chn->owner = tinfo;
	lm_map_put(&(a->chn_to_ch_info), chnum, (ur_map_value_type)chn);

	lm_map_put(&(tinfo->chns), (ur_map_key_type) addr_get_port(peer_addr), (ur_map_value_type) chn);

	return chn;
}

ch_info* allocation_get_ch_info(allocation* a, u16bits chnum) {
	ur_map_value_type vchn = 0;
	if (lm_map_get(&(a->chn_to_ch_info), chnum, &vchn) && vchn) {
		return (ch_info*) vchn;
	}
	return NULL;
}

ch_info* allocation_get_ch_info_by_peer_addr(allocation* a, ioa_addr* peer_addr) {
	turn_permission_info* tinfo = get_from_turn_permission_hashtable(&(a->addr_to_perm), peer_addr);
	if(tinfo) {
		return get_turn_channel(tinfo,peer_addr);
	}
	return NULL;
}

u16bits get_turn_channel_number(turn_permission_info* tinfo, ioa_addr *addr)
{
	if (tinfo) {
		ur_map_value_type t = 0;
		if (lm_map_get(&(tinfo->chns), (ur_map_key_type)addr_get_port(addr), &t) && t) {
			ch_info* chn = (ch_info*) t;
			if (STUN_VALID_CHANNEL(chn->chnum)) {
				return chn->chnum;
			}
		}
	}

	return 0;
}

ch_info *get_turn_channel(turn_permission_info* tinfo, ioa_addr *addr)
{
	if (tinfo) {
		ur_map_value_type t = 0;
		if (lm_map_get(&(tinfo->chns), (ur_map_key_type)addr_get_port(addr), &t) && t) {
			ch_info* chn = (ch_info*) t;
			if (STUN_VALID_CHANNEL(chn->chnum)) {
				return chn;
			}
		}
	}

	return NULL;
}

turn_permission_hashtable *allocation_get_turn_permission_hashtable(allocation *a)
{
  return &(a->addr_to_perm);
}

turn_permission_info* allocation_add_permission(allocation *a, const ioa_addr* addr)
{
	if (a && addr) {

		turn_permission_hashtable *map = &(a->addr_to_perm);
		u32bits hash = addr_hash_no_port(addr);
		size_t fds = (size_t) (hash & (TURN_PERMISSION_HASHTABLE_SIZE-1));

		turn_permission_array *parray = &(map->table[fds]);

		turn_permission_slot *slot = NULL;

		{
			size_t i;
			for(i=0;i<TURN_PERMISSION_ARRAY_SIZE;++i) {
				slot = &(parray->main_slots[i]);
				if(!(slot->info.allocated)) {
					break;
				} else {
					slot=NULL;
				}
			}
		}

		if(!slot) {

			size_t old_sz = parray->extra_sz;

			turn_permission_slot **slots = parray->extra_slots;

			if(slots) {
				size_t i;
				for(i=0;i<old_sz;++i) {
					slot = slots[i];
					if(!(slot->info.allocated)) {
						break;
					} else {
						slot=NULL;
					}
				}
			}

			if(!slot) {
				size_t old_sz_mem = old_sz * sizeof(turn_permission_slot*);
				parray->extra_slots = (turn_permission_slot **) turn_realloc(parray->extra_slots,
						old_sz_mem, old_sz_mem + sizeof(turn_permission_slot*));
				slots = parray->extra_slots;
				parray->extra_sz = old_sz + 1;
				slots[old_sz] = (turn_permission_slot *)turn_malloc(sizeof(turn_permission_slot));
				slot = slots[old_sz];
			}
		}

		ns_bzero(slot,sizeof(turn_permission_slot));
		slot->info.allocated = 1;
		turn_permission_info *elem = &(slot->info);
		addr_cpy(&(elem->addr), addr);
		elem->owner = a;

		return elem;
	} else {
		return NULL;
	}
}

////////////////// TCP connections ///////////////////////////////

static void set_new_tc_id(u08bits server_id, tcp_connection *tc) {
	allocation *a = (allocation*)(tc->owner);
	ur_map *map = a->tcp_connections;
	u32bits newid = 0;
	u32bits sid = server_id;
	sid = sid<<24;
	do {
		while (!newid) {
			newid = (u32bits)random();
			if(!newid) {
				continue;
			}
			newid = newid & 0x00FFFFFF;
			if(!newid) {
				continue;
			}
			newid = newid | sid;
		}
	} while(ur_map_get(map, (ur_map_key_type)newid, NULL));
	tc->id = newid;
	ur_map_put(map, (ur_map_key_type)newid, (ur_map_value_type)tc);
}

tcp_connection *create_tcp_connection(u08bits server_id, allocation *a, stun_tid *tid, ioa_addr *peer_addr, int *err_code)
{
	tcp_connection_list *tcl = &(a->tcl);
	while(tcl->next) {
		tcp_connection *otc = (tcp_connection*)(tcl->next);
		if(addr_eq(&(otc->peer_addr),peer_addr)) {
			*err_code = 446;
			return NULL;
		}
		tcl=tcl->next;
	}
	tcp_connection *tc = (tcp_connection*)turn_malloc(sizeof(tcp_connection));
	ns_bzero(tc,sizeof(tcp_connection));
	tcl->next = &(tc->list);
	addr_cpy(&(tc->peer_addr),peer_addr);
	if(tid)
		ns_bcopy(tid,&(tc->tid),sizeof(stun_tid));
	tc->owner = a;
	set_new_tc_id(server_id, tc);
	return tc;
}

void delete_tcp_connection(tcp_connection *tc)
{
	if(tc) {
		if(tc->done) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "!!! %s: check on already closed tcp data connection: 0x%lx\n",__FUNCTION__,(unsigned long)tc);
			return;
		}
		tc->done = 1;

		clear_unsent_buffer(&(tc->ub_to_client));

		IOA_EVENT_DEL(tc->peer_conn_timeout);
		IOA_EVENT_DEL(tc->conn_bind_timeout);
		allocation *a = (allocation*)(tc->owner);
		if(a) {
			ur_map *map = a->tcp_connections;
			if(map) {
				ur_map_del(map, (ur_map_key_type)(tc->id),NULL);
			}
			tcp_connection_list *tcl = &(a->tcl);
			while(tcl->next) {
				if((void*)(tcl->next) == (void*)tc) {
					tcl->next = tc->list.next;
					break;
				} else {
					tcl=tcl->next;
				}
			}
		}
		set_ioa_socket_sub_session(tc->client_s,NULL);
		IOA_CLOSE_SOCKET(tc->client_s);
		set_ioa_socket_sub_session(tc->peer_s,NULL);
		IOA_CLOSE_SOCKET(tc->peer_s);
		turn_free(tc,sizeof(tcp_connection));
	}
}

tcp_connection *get_and_clean_tcp_connection_by_id(ur_map *map, tcp_connection_id id)
{
	if(map) {
		ur_map_value_type t = 0;
		if (ur_map_get(map, (ur_map_key_type)id, &t) && t) {
			ur_map_del(map, (ur_map_key_type)id,NULL);
			return (tcp_connection*)t;
		}
	}
	return NULL;
}

tcp_connection *get_tcp_connection_by_peer(allocation *a, ioa_addr *peer_addr)
{
	if(a && peer_addr) {
		tcp_connection_list *tcl = &(a->tcl);
		while(tcl->next) {
			tcp_connection *tc = (tcp_connection*)(tcl->next);
			if(addr_eq(&(tc->peer_addr),peer_addr)) {
				return tc;
			}
			tcl=tcl->next;
		}
	}
	return NULL;
}

int can_accept_tcp_connection_from_peer(allocation *a, ioa_addr *peer_addr, int server_relay)
{
	if(server_relay)
		return 1;

	if(a && peer_addr) {
		return (get_from_turn_permission_hashtable(&(a->addr_to_perm), peer_addr) != NULL);
	}

	return 0;
}

//////////////// Unsent buffers //////////////////////

void clear_unsent_buffer(unsent_buffer *ub)
{
	if(ub) {
		if(ub->bufs) {
			size_t sz;
			for(sz = 0; sz<ub->sz; sz++) {
				ioa_network_buffer_handle nbh = ub->bufs[sz];
				if(nbh) {
					ioa_network_buffer_delete(NULL, nbh);
					ub->bufs[sz] = NULL;
				}
			}
			turn_free(ub->bufs,sizeof(ioa_network_buffer_handle) * ub->sz);
			ub->bufs = NULL;
		}
		ub->sz = 0;
	}
}

void add_unsent_buffer(unsent_buffer *ub, ioa_network_buffer_handle nbh)
{
	if(!ub || (ub->sz >= MAX_UNSENT_BUFFER_SIZE)) {
		ioa_network_buffer_delete(NULL, nbh);
	} else {
		ub->bufs = (ioa_network_buffer_handle*)turn_realloc(ub->bufs, sizeof(ioa_network_buffer_handle) * ub->sz, sizeof(ioa_network_buffer_handle) * (ub->sz+1));
		ub->bufs[ub->sz] = nbh;
		ub->sz +=1;
	}
}

ioa_network_buffer_handle top_unsent_buffer(unsent_buffer *ub)
{
	ioa_network_buffer_handle ret = NULL;
	if(ub && ub->bufs && ub->sz) {
		size_t sz;
		for(sz=0; sz<ub->sz; ++sz) {
			if(ub->bufs[sz]) {
				ret = ub->bufs[sz];
				break;
			}
		}
	}
	return ret;
}

void pop_unsent_buffer(unsent_buffer *ub)
{
	if(ub && ub->bufs && ub->sz) {
		size_t sz;
		for(sz=0; sz<ub->sz; ++sz) {
			if(ub->bufs[sz]) {
				ub->bufs[sz] = NULL;
				break;
			}
		}
	}
}

//////////////////////////////////////////////////////////////////

