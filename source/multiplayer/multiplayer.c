#include "multiplayer.h"
#include "platform/platform_profile.h"
#include <SDL2/SDL.h>
#include <switch.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
/* Wire format: magic(4), version(1), type(1), length(2), match(8),
sequence(4), payload. Integers are explicitly big endian. TCP provides ACK,
ordering and retransmission. Queues and peer buffers have fixed capacities. */
#define FRAME_MAX 1024
#define QUEUE_MAX 32
#define LOCAL_PORT 42000
#define LOCAL_SCENE 0x434e
#define DEAD_MS 12000
#define FINAL_MS 2000
#define PROTOCOL_VERSION 5
#define PUZZLE_WIRE_SIZE 808
#define PREPARE_MS 120000
enum {
    W_HELLO=1, W_STATE, W_READY, W_SCORE, W_PING, W_ERROR, W_COUNT, W_SOLVED, W_ICON, W_RACE_BEGIN, W_RACE_PUZZLE, W_RACE_END, W_PROGRESS
};
enum {
    C_LOCAL=1, C_WIFI, C_STOP, C_RULES, C_PREPARE, C_READY, C_SCORE, C_COUNT, C_SOLVED, C_APPEARANCE, C_RETURN_LOBBY, C_LIBRARY, C_LOCAL_JOIN, C_PROGRESS
};
typedef struct {
    int type;
    bool host, ready;
    uint16_t port;
    char ip[48], name[33];
    MpRules rules;
    uint32_t score;
    uint64_t elapsed;
    uint8_t color_id, icon_rgba[MP_ICON_BYTES];
    uint32_t profile_color;
    ConnectionsPuzzle *library;
}
Command;
typedef struct {
    int fd, player;
    bool connecting, state_pending;
    uint8_t rx[FRAME_MAX], tx[FRAME_MAX*4];
    size_t rn, tn, sent;
    uint64_t last_rx, joined_at;
    uint64_t icon_ids[MP_MAX_PLAYERS], rx_icon_id;
    uint32_t icon_versions[MP_MAX_PLAYERS], icon_offsets[MP_MAX_PLAYERS];
    uint32_t rx_icon_version, rx_icon_offset, rx_profile_color;
    uint8_t rx_icon_color, rx_icon[MP_ICON_BYTES];
    uint64_t race_tx_match;
    unsigned race_tx_index;
    bool race_begin_sent, race_end_sent;
}
Peer;
typedef struct {
    bool (*begin)(const Command *);
    void (*stop)(void);
}
IMultiplayerTransport;
static SDL_Thread *thread;
static SDL_mutex *queue_lock, *snapshot_lock, *race_lock;
static Command queue[QUEUE_MAX];
static unsigned qread, qcount;
static atomic_bool quit;
static atomic_uint revision;
static unsigned read_revision;
static MpSnapshot published, state;
static Peer peers[MP_MAX_PLAYERS-1];
static int listener=-1;
static bool sockets_up, ldn_up, local_mode, dirty;
static uint64_t phase_deadline, next_ping, connect_deadline, own_id;
static uint32_t local_puzzle_count;
static uint32_t local_solved_count;
static uint8_t local_color, local_icon[MP_ICON_BYTES];
static uint32_t local_icon_revision;
static uint32_t local_profile_color=0xff707070;
static MpPlayer previous_players[MP_MAX_PLAYERS];
static uint32_t own_sequence, score_sequences[MP_MAX_PLAYERS];
static Mutex socket_lifetime_lock;
static unsigned socket_users;
/* Worker owns libraries/build buffers; UI copies a single published puzzle. */
static ConnectionsPuzzle *host_library, *cached_race, *receiving_race;
static unsigned host_library_count, cached_race_count, receiving_count, receiving_index;
static uint64_t cached_race_match, receiving_match;
static uint8_t race_digest[32];
static Sha256Context receiving_hash;
static LdnNetworkInfo discovered_networks[MP_LOCAL_LOBBIES];
static uint64_t local_session_id, next_advertise;
static bool local_host;
static uint32_t race_random(uint32_t *seed);
static void local_advertise(void);
static void queue_race(Peer *peer);
static void lost_peer(Peer *p);
static bool drop_peer(Peer *p);
bool mp_socket_acquire(void) {
    mutexLock(&socket_lifetime_lock);
    bool ok=socket_users!=0 || R_SUCCEEDED(socketInitializeDefault());
    if(ok) socket_users++;
    mutexUnlock(&socket_lifetime_lock);
    return ok;
}
void mp_socket_release(void) {
    mutexLock(&socket_lifetime_lock);
    if(socket_users && !--socket_users) socketExit();
    mutexUnlock(&socket_lifetime_lock);
}
static uint64_t now_ms(void) {
    return SDL_GetPerformanceCounter()*1000/SDL_GetPerformanceFrequency();
}
static uint64_t ticks_after(uint64_t ms) {
    return SDL_GetPerformanceCounter()+ms*SDL_GetPerformanceFrequency()/1000;
}
static void put32(uint8_t *p,uint32_t v) {
    p[0]=v>>24;
    p[1]=v>>16;
    p[2]=v>>8;
    p[3]=v;
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}
static void put64(uint8_t *p,uint64_t v) {
    put32(p,v>>32);
    put32(p+4,v);
}
static uint64_t get64(const uint8_t *p) {
    return (uint64_t)get32(p)<<32|get32(p+4);
}
static void publish(void) {
    if (!dirty) return;
    SDL_LockMutex(snapshot_lock);
    published=state;
    SDL_UnlockMutex(snapshot_lock);
    atomic_fetch_add_explicit(&revision,1,memory_order_release);
    dirty=false;
}
static bool enqueue(Command c) {
    if (!thread) return false;
    SDL_LockMutex(queue_lock);
    bool ok=qcount<QUEUE_MAX;
    if (ok) {
        queue[(qread+qcount)%QUEUE_MAX]=c;
        qcount++;
    }
    SDL_UnlockMutex(queue_lock);
    return ok;
}
static bool dequeue(Command *c) {
    SDL_LockMutex(queue_lock);
    bool ok=qcount!=0;
    if(ok) {
        *c=queue[qread];
        qread=(qread+1)%QUEUE_MAX;
        qcount--;
    }
    SDL_UnlockMutex(queue_lock);
    return ok;
}
static void close_peer(Peer *p) {
    if(p->fd>=0) close(p->fd);
    memset(p,0,sizeof(*p));
    p->fd=-1;
    p->player=-1;
}
static void transport_stop(void) {
    for(unsigned i=0;i<MP_MAX_PLAYERS-1;i++) close_peer(&peers[i]);
    if(listener>=0) close(listener);
    listener=-1;
    if(ldn_up) {
        LdnState mode=LdnState_None;
        if(R_SUCCEEDED(ldnGetState(&mode))) {
            if(mode==LdnState_StationConnected) { ldnDisconnect(); ldnCloseStation(); }
            else if(mode==LdnState_Station) ldnCloseStation();
            else if(mode==LdnState_AccessPointCreated) { ldnDestroyNetwork(); ldnCloseAccessPoint(); }
            else if(mode==LdnState_AccessPoint) ldnCloseAccessPoint();
        }
        ldnExit();
        ldn_up=false;
    }
    if(sockets_up) {
        mp_socket_release();
        sockets_up=false;
    }
}
static void fail(const char *error) {
    transport_stop();
    state.state=MP_ERROR;
    snprintf(state.error,sizeof(state.error),"%s",error);
    dirty=true;
}
static void nonblock(int fd) {
    fcntl(fd,F_SETFL,fcntl(fd,F_GETFL,0)|O_NONBLOCK);
}
static bool send_frame(Peer *p,int type,uint32_t seq,const uint8_t *payload,size_t len) {
    if(p->fd<0 || len>FRAME_MAX-20) return false;
    if(p->sent) {
        memmove(p->tx,p->tx+p->sent,p->tn-p->sent);
        p->tn-=p->sent;
        p->sent=0;
    }
    if(p->tn+20+len>sizeof(p->tx)) return false;
    uint8_t *b=p->tx+p->tn;
    memcpy(b,"CNR1",4);
    b[4]=PROTOCOL_VERSION;
    b[5]=type;
    b[6]=len>>8;
    b[7]=len;
    put64(b+8,state.match_id);
    put32(b+16,seq);
    if(len)memcpy(b+20,payload,len);
    p->tn+=20+len;
    return true;
}
/* Scoreboards/membership are replaceable snapshots, not an unbounded stream.
   Coalesce changes and encode the latest state only when the peer has room. */
static bool send_state(Peer *peer) {
    uint8_t b[700];
    size_t n=0;
    b[n++]=state.state;
    b[n++]=state.player_count;
    b[n++]=state.winner_mask;
    b[n++]=state.rules.mode;
    put32(b+n,state.rules.duration_seconds);
    n+=4;
    put32(b+n,state.rules.target_score);
    n+=4;
    int32_t timing=0;
    uint64_t now=SDL_GetPerformanceCounter();
    if(state.start_ticks>now)timing=(int32_t)((state.start_ticks-now)*1000/SDL_GetPerformanceFrequency());
    else if(state.start_ticks)timing=-(int32_t)((now-state.start_ticks)*1000/SDL_GetPerformanceFrequency());
    put32(b+n,(uint32_t)timing);
    n+=4;
    for(unsigned i=0;i<state.player_count;i++) {
        MpPlayer *p=&state.players[i];
        put64(b+n,p->id);
        n+=8;
        put32(b+n,p->score);
        n+=4;
        put64(b+n,p->elapsed_us);
        n+=8;
        b[n++]=p->connected;
        b[n++]=p->ready;
        memcpy(b+n,p->name,33);
        n+=33;
        put32(b+n,p->puzzle_count);
        n+=4;
        put32(b+n,p->solved_count);
        n+=4;
        b[n++]=p->color_id;
        put32(b+n,p->profile_color);
        n+=4;
        put32(b+n,p->icon_revision);
        n+=4;
        put32(b+n,p->puzzle_index);
        n+=4;
    }
    return send_frame(peer,W_STATE,0,b,n);
}
static void queue_state(Peer *peer) {
    if(!peer->state_pending||peer->fd<0||peer->player<0||peer->connecting)return;
    size_t size=20+16+state.player_count*76;
    if(peer->tn-peer->sent+size<=sizeof(peer->tx)&&send_state(peer))peer->state_pending=false;
}
static void broadcast_state(void) {
    for(unsigned i=0;i<MP_MAX_PLAYERS-1;i++)if(peers[i].fd>=0&&peers[i].player>=0&&!peers[i].connecting)
        peers[i].state_pending=true;
    dirty=true;
    local_advertise();
}
static bool drop_peer(Peer *p) {
    int idx=p->player;
    close_peer(p);
    if(!state.is_host) {
        fail("Host disconnected");
        return false;
    }
    if(idx>=0 && idx<(int)state.player_count) {
        if(state.state==MP_LOBBY) {
            memmove(state.players+idx,state.players+idx+1,(state.player_count-idx-1)*sizeof(MpPlayer));
            memmove(score_sequences+idx,score_sequences+idx+1,(state.player_count-idx-1)*sizeof(uint32_t));
            state.player_count--;
            for(unsigned i=0;i<MP_MAX_PLAYERS-1;i++) if(peers[i].player>idx) peers[i].player--;
        }
        else { state.players[idx].connected=false; state.players[idx].ready=false; }
        return true;
    }
    return false;
}
static void lost_peer(Peer *p) {
    if(drop_peer(p)) broadcast_state();
}
static bool setup_socket(bool host,const char *ip,uint16_t port) {
    if(!sockets_up) {
        if(!mp_socket_acquire())return false;
        sockets_up=true;
    }
    int fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0)return false;
    nonblock(fd);
    struct sockaddr_in a= {
        0
    };
    a.sin_family=AF_INET;
    a.sin_port=htons(port);
    a.sin_addr.s_addr=INADDR_ANY;
    if(host) {
        int reuse=1;
        setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
        if(bind(fd,(struct sockaddr*)&a,sizeof(a))<0||listen(fd,MP_MAX_PLAYERS-1)<0) {
            close(fd);
            return false;
        }
        listener=fd;
        state.is_host=true;
        state.state=MP_LOBBY;
        state.player_count=1;
        state.players[0].id=own_id;
        state.players[0].connected=true;
        // Discovery may outlast library/profile preparation. Use the latest
        // worker-owned values when admitting the host to its own lobby.
        state.players[0].puzzle_count=local_puzzle_count;
        state.players[0].solved_count=local_solved_count;
        state.players[0].color_id=local_color;
        state.players[0].profile_color=local_profile_color;
        state.players[0].icon_revision=local_icon_revision;
        memcpy(state.players[0].icon_rgba,local_icon,MP_ICON_BYTES);
    }
    else {
        if(inet_pton(AF_INET,ip,&a.sin_addr)!=1) {
            close(fd);
            return false;
        }
        int rc=connect(fd,(struct sockaddr*)&a,sizeof(a));
        if(rc<0&&errno!=EINPROGRESS&&errno!=EWOULDBLOCK) {
            close(fd);
            return false;
        }
        Peer *p=&peers[0];
        p->fd=fd;
        p->connecting=rc<0;
        p->last_rx=now_ms();
        state.state=MP_CONNECTING;
        connect_deadline=now_ms()+10000;
        if(!p->connecting) {
            uint8_t hello[54];
            put64(hello,own_id);
            memcpy(hello+8,state.players[0].name,33);
            put32(hello+41,local_puzzle_count);
            put32(hello+45,local_solved_count);
            hello[49]=local_color;
            put32(hello+50,local_profile_color);
            send_frame(p,W_HELLO,0,hello,sizeof(hello));
        }
    }
    dirty=true;
    return true;
}
static bool wifi_begin(const Command *c) {
    local_mode=false;
    return setup_socket(c->host,c->ip,c->port);
}
static void local_security(LdnSecurityConfig *sec,LdnUserConfig *user) {
    memset(sec,0,sizeof(*sec)); memset(user,0,sizeof(*user));
    sec->security_mode=LdnSecurityMode_Product;
    sec->passphrase_size=16;
    memcpy(sec->passphrase,"ConnectionsRace1",16);
    memcpy(user->user_name,state.players[0].name,33);
}
static void local_advertise(void) {
    if(!ldn_up||!local_host||!state.is_host)return;
    uint8_t ad[49]={0};
    memcpy(ad,"CN5L",4); put64(ad+4,local_session_id);
    memcpy(ad+12,state.players[0].name,33);
    unsigned count=0;
    for(unsigned i=0;i<state.player_count;i++)if(state.players[i].connected)++count;
    ad[45]=(uint8_t)count; ad[46]=MP_MAX_PLAYERS;
    ad[47]=state.state!=MP_LOBBY; ad[48]=PROTOCOL_VERSION;
    ldnSetAdvertiseData(ad,sizeof(ad));
}
static bool local_begin(const Command *c) {
    local_mode=true; local_host=c->host;
    state.local_lobby_count=0;
    if(R_FAILED(ldnInitialize(LdnServiceType_User)))return false;
    ldn_up=true;
    if(!c->host) {
        if(R_FAILED(ldnOpenStation()))return false;
        state.state=MP_DISCOVERING; dirty=true;
        return true;
    }
    LdnSecurityConfig sec; LdnUserConfig user;
    local_security(&sec,&user);
    LdnNetworkConfig config={0};
    config.intent_id.local_communication_id=-1;
    config.intent_id.scene_id=LOCAL_SCENE;
    config.node_count_max=MP_MAX_PLAYERS;
    config.local_communication_version=PROTOCOL_VERSION;
    local_session_id=SDL_GetPerformanceCounter()^own_id;
    if(!local_session_id)local_session_id=own_id;
    if(R_FAILED(ldnOpenAccessPoint())||R_FAILED(ldnCreateNetwork(&sec,&user,&config))||
       !setup_socket(true,"",LOCAL_PORT))return false;
    local_advertise();
    return true;
}
static const IMultiplayerTransport WifiTransport={wifi_begin,transport_stop};
static const IMultiplayerTransport LocalWirelessTransport={local_begin,transport_stop};
static void local_discover(void) {
    LdnScanFilter filter={0};
    filter.flags=LdnScanFilterFlag_SceneId;
    filter.network_id.intent_id.scene_id=LOCAL_SCENE;
    LdnNetworkInfo nets[MP_LOCAL_LOBBIES]={0}; s32 count=0;
    if(R_FAILED(ldnScan(0,&filter,nets,MP_LOCAL_LOBBIES,&count))) {
        fail("Local discovery unavailable"); return;
    }
    MpLocalLobby visible[MP_LOCAL_LOBBIES]={0}; unsigned found=0;
    for(int i=0;i<count&&found<MP_LOCAL_LOBBIES;i++) {
        const uint8_t *ad=(const uint8_t *)nets[i].advertise_data;
        if(nets[i].advertise_data_size!=49||memcmp(ad,"CN5L",4)||ad[48]!=PROTOCOL_VERSION||ad[44]||
           !get64(ad+4)||ad[46]!=MP_MAX_PLAYERS||ad[45]>MP_MAX_PLAYERS||ad[47]>1)continue;
        MpLocalLobby *lobby=&visible[found];
        lobby->id=get64(ad+4); memcpy(lobby->name,ad+12,33);
        lobby->players=ad[45]; lobby->max_players=ad[46];
        lobby->playing=ad[47]!=0; lobby->full=ad[45]>=ad[46];
        discovered_networks[found]=nets[i]; ++found;
    }
    if(state.local_lobby_count!=found||memcmp(state.local_lobbies,visible,sizeof(visible))) {
        state.local_lobby_count=found;
        memcpy(state.local_lobbies,visible,sizeof(visible)); dirty=true;
    }
}
static void local_join(uint64_t id) {
    if(!local_mode||state.state!=MP_DISCOVERING)return;
    for(unsigned i=0;i<state.local_lobby_count;i++)if(state.local_lobbies[i].id==id) {
        if(state.local_lobbies[i].full) {snprintf(state.error,sizeof(state.error),"Lobby full");dirty=true;return;}
        LdnSecurityConfig sec; LdnUserConfig user;
        local_security(&sec,&user);
        if(R_FAILED(ldnConnect(&sec,&user,PROTOCOL_VERSION,0,&discovered_networks[i]))) {
            snprintf(state.error,sizeof(state.error),"Unable to join local session");dirty=true;return;
        }
        struct in_addr addr={htonl(discovered_networks[i].nodes[0].ip_addr.addr)};
        char ip[48];snprintf(ip,sizeof(ip),"%s",inet_ntoa(addr));
        state.error[0]=0;
        if(!setup_socket(false,ip,LOCAL_PORT))fail("Unable to connect locally");
        return;
    }
    snprintf(state.error,sizeof(state.error),"Session no longer available");dirty=true;
}
static void clear_race(void) {
    SDL_LockMutex(race_lock);
    ConnectionsPuzzle *old=cached_race;
    cached_race=NULL; cached_race_count=0; cached_race_match=0;
    SDL_UnlockMutex(race_lock);
    free(old); free(receiving_race); receiving_race=NULL;
    receiving_count=receiving_index=0; receiving_match=0;
    state.race_ready=false; state.race_count=0;
}
static void publish_race(ConnectionsPuzzle *puzzles,unsigned count,uint64_t match) {
    SDL_LockMutex(race_lock);
    ConnectionsPuzzle *old=cached_race;
    cached_race=puzzles; cached_race_count=count; cached_race_match=match;
    SDL_UnlockMutex(race_lock);
    free(old);
    state.race_ready=true; state.race_count=count; dirty=true;
}
/* Fixed-width, explicit wire fields; answers are reconstructed from group IDs. */
static void encode_puzzle(uint8_t *b,const ConnectionsPuzzle *p) {
    put32(b,(uint32_t)p->id); memcpy(b+4,p->date,16);
    memcpy(b+20,p->words,16*32); memcpy(b+532,p->group_names,4*64);
    for(unsigned i=0;i<4;i++)b[788+i]=(uint8_t)p->group_levels[i];
    for(unsigned i=0;i<16;i++)b[792+i]=(uint8_t)p->group_ids[i];
}
static bool wire_text(const uint8_t *p,unsigned size) {
    const uint8_t *end=memchr(p,0,size);
    if(!end||end==p)return false;
    for(const uint8_t *q=p;q<end;q++)if(*q<32)return false;
    return true;
}
static bool decode_puzzle(const uint8_t *b,ConnectionsPuzzle *p) {
    if(get32(b)>INT_MAX||!wire_text(b+4,16))return false;
    memset(p,0,sizeof(*p)); p->id=(int)get32(b); memcpy(p->date,b+4,16);
    unsigned groups[4]={0};
    for(unsigned i=0;i<4;i++) {
        if(!wire_text(b+532+i*64,64)||(b[788+i]>3&&b[788+i]!=255))return false;
        memcpy(p->group_names[i],b+532+i*64,64);p->group_levels[i]=(int8_t)b[788+i];
    }
    for(unsigned i=0;i<16;i++) {
        unsigned group=b[792+i];
        if(group>=4||groups[group]>=4||!wire_text(b+20+i*32,32))return false;
        memcpy(p->words[i],b+20+i*32,32);p->group_ids[i]=(int)group;
        memcpy(p->answers[group][groups[group]++],b+20+i*32,32);
    }
    p->word_count=16;return true;
}
static bool prepare_host_race(void) {
    unsigned count=host_library_count;
    for(unsigned i=0;i<state.player_count;i++)if(state.players[i].connected&&state.players[i].puzzle_count<count)
        count=state.players[i].puzzle_count;
    if(!host_library||!count||count>MP_MAX_PUZZLES)return false;
    for(unsigned i=0;i<host_library_count;i++)if(host_library[i].word_count!=CONNECTIONS_WORD_COUNT)return false;
    ConnectionsPuzzle *puzzles=calloc(count,sizeof(*puzzles));
    uint32_t *order=malloc(host_library_count*sizeof(*order));
    if(!puzzles||!order) {free(puzzles);free(order);return false;}
    uint32_t seed=(uint32_t)(state.match_id^(state.match_id>>32));if(!seed)seed=1;
    for(unsigned i=0;i<host_library_count;i++)order[i]=i;
    for(unsigned i=host_library_count-1;i>0;i--) {
        unsigned j=race_random(&seed)%(i+1);uint32_t temp=order[i];order[i]=order[j];order[j]=temp;
    }
    Sha256Context hash;sha256ContextCreate(&hash);
    for(unsigned i=0;i<count;i++) {
        puzzles[i]=host_library[order[i]];
        for(unsigned j=15;j>0;j--) {
            unsigned k=race_random(&seed)%(j+1);char word[32];
            memcpy(word,puzzles[i].words[j],32);memcpy(puzzles[i].words[j],puzzles[i].words[k],32);
            memcpy(puzzles[i].words[k],word,32);
            int group=puzzles[i].group_ids[j];puzzles[i].group_ids[j]=puzzles[i].group_ids[k];puzzles[i].group_ids[k]=group;
        }
        uint8_t wire[PUZZLE_WIRE_SIZE];encode_puzzle(wire,&puzzles[i]);sha256ContextUpdate(&hash,wire,sizeof(wire));
    }
    free(order);sha256ContextGetHash(&hash,race_digest);
    publish_race(puzzles,count,state.match_id);return true;
}
static void queue_race(Peer *peer) {
    if(!state.is_host||peer->connecting||peer->player<0||!state.race_ready||state.state<MP_PREPARING||state.state>MP_RESULTS||peer->tn-peer->sent>512)return;
    if(peer->race_tx_match!=state.match_id) {
        peer->race_tx_match=state.match_id;peer->race_tx_index=0;
        peer->race_begin_sent=peer->race_end_sent=false;
    }
    if(!peer->race_begin_sent) {
        uint8_t begin[36];put32(begin,cached_race_count);memcpy(begin+4,race_digest,32);
        if(send_frame(peer,W_RACE_BEGIN,0,begin,sizeof(begin)))peer->race_begin_sent=true;
    } else if(peer->race_tx_index<cached_race_count) {
        uint8_t item[4+PUZZLE_WIRE_SIZE];put32(item,peer->race_tx_index);
        encode_puzzle(item+4,&cached_race[peer->race_tx_index]);
        if(send_frame(peer,W_RACE_PUZZLE,0,item,sizeof(item)))++peer->race_tx_index;
    } else if(!peer->race_end_sent) {
        if(send_frame(peer,W_RACE_END,0,race_digest,32))peer->race_end_sent=true;
    }
}
static bool receive_race(int type,uint64_t match,const uint8_t *b,size_t n) {
    if(state.is_host||!match||match!=state.match_id)return false;
    if(type==W_RACE_BEGIN) {
        if(n!=36||!get32(b)||get32(b)>MP_MAX_PUZZLES)return false;
        clear_race();receiving_count=get32(b);receiving_match=match;
        receiving_race=calloc(receiving_count,sizeof(*receiving_race));
        if(!receiving_race)return false;
        memcpy(race_digest,b+4,32);sha256ContextCreate(&receiving_hash);
        state.race_count=receiving_count;dirty=true;return true;
    }
    if(!receiving_race||receiving_match!=match)return false;
    if(type==W_RACE_PUZZLE) {
        if(n!=4+PUZZLE_WIRE_SIZE||get32(b)!=receiving_index||receiving_index>=receiving_count||
           !decode_puzzle(b+4,&receiving_race[receiving_index]))return false;
        sha256ContextUpdate(&receiving_hash,b+4,PUZZLE_WIRE_SIZE);++receiving_index;return true;
    }
    if(type==W_RACE_END) {
        uint8_t digest[32];sha256ContextGetHash(&receiving_hash,digest);
        if(n!=32||receiving_index!=receiving_count||memcmp(digest,b,32)||memcmp(digest,race_digest,32))return false;
        ConnectionsPuzzle *ready=receiving_race;receiving_race=NULL;
        publish_race(ready,receiving_count,match);receiving_count=receiving_index=0;receiving_match=0;
        return true;
    }
    return false;
}
static bool valid_rules(MpRules r) {
    return (r.mode==MP_TIMED||r.mode==MP_TARGET_SCORE)&&r.duration_seconds>=30&&r.duration_seconds<=3600&&r.target_score>=1&&r.target_score<=MP_MAX_PUZZLES;
}
/* Zero means at least one connected device has not loaded its library yet. */
static uint32_t puzzle_limit(void) {
    uint32_t limit=MP_MAX_PUZZLES;
    for(unsigned i=0;i<state.player_count;i++) if(state.players[i].connected) {
        if(state.players[i].puzzle_count<limit)limit=state.players[i].puzzle_count;
    }
    return limit;
}
static void cap_rules(void) {
    uint32_t limit=puzzle_limit();
    if(limit&&state.rules.target_score>limit)state.rules.target_score=limit;
}
static void maybe_start(void) {
    if(!state.is_host||state.state!=MP_PREPARING)return;
    uint32_t limit=puzzle_limit();
    if(!limit||(state.rules.mode==MP_TARGET_SCORE&&state.rules.target_score>limit))return;
    for(unsigned i=0;i<state.player_count;i++) if(state.players[i].connected&&!state.players[i].ready)return;
    state.state=MP_STARTING;
    state.start_ticks=ticks_after(1500);
    broadcast_state();
}
static void score(int idx,uint32_t seq,uint32_t value,uint64_t elapsed) {
    if(idx<0||idx>=(int)state.player_count||!state.players[idx].connected||(state.state!=MP_PLAYING&&state.state!=MP_FINALIZING))return;
    MpPlayer *p=&state.players[idx];
    if(!p->ready)return;
    uint64_t host_elapsed=(SDL_GetPerformanceCounter()-state.start_ticks)*1000000/SDL_GetPerformanceFrequency();
    if(seq<=score_sequences[idx]||value!=p->score+1||elapsed<p->elapsed_us||elapsed>host_elapsed+2000000||value>1000000)return;
    if(state.rules.mode==MP_TIMED&&elapsed>(uint64_t)state.rules.duration_seconds*1000000)return;
    if(state.rules.mode==MP_TARGET_SCORE&&value>state.rules.target_score)return;
    score_sequences[idx]=seq;
    p->score=value;
    p->elapsed_us=elapsed;
    if(state.rules.mode==MP_TARGET_SCORE&&value>=state.rules.target_score&&state.state==MP_PLAYING) {
        state.state=MP_FINALIZING;
        phase_deadline=now_ms()+FINAL_MS;
    }
    broadcast_state();
}
static void finish(void) {
    uint32_t best=0;
    uint64_t earliest=UINT64_MAX;
    unsigned winners=0;
    /* Tied final score: earliest match-relative qualifying completion wins.
    Exact elapsed ties (including all-zero scores) produce multiple winners. */
    for(unsigned i=0;i<state.player_count;i++) {
        MpPlayer *p=&state.players[i];
        if(!p->connected)continue;
        if(p->score>best||(p->score==best&&p->elapsed_us<earliest)) {
            best=p->score;
            earliest=p->elapsed_us;
            winners=1u<<i;
        }
        else if(p->score==best&&p->elapsed_us==earliest)winners|=1u<<i;
    }
    state.winner_mask=winners;
    state.state=MP_RESULTS;
    broadcast_state();
}
/* One small chunk per worker pass leaves most TX capacity for gameplay events.
   TCP ordering permits a single bounded receive staging buffer per connection. */
static void queue_icon_chunk(Peer *peer) {
    if(peer->connecting||peer->tn-peer->sent>1024)return;
    if(state.is_host&&peer->player<0)return;
    if(!state.is_host&&state.state==MP_CONNECTING)return;
    for(unsigned i=0;i<state.player_count;i++) {
        MpPlayer *p=&state.players[i];
        if(!state.is_host&&p->id!=own_id)continue;
        if(!p->icon_revision)continue;
        if(peer->icon_ids[i]!=p->id||peer->icon_versions[i]!=p->icon_revision) {
            peer->icon_ids[i]=p->id;
            peer->icon_versions[i]=p->icon_revision;
            peer->icon_offsets[i]=0;
        }
        uint32_t offset=peer->icon_offsets[i];
        if(offset>=MP_ICON_BYTES)continue;
        uint8_t payload[21+512];
        put64(payload,p->id);
        put32(payload+8,p->icon_revision);
        put32(payload+12,offset);
        payload[16]=p->color_id;
        put32(payload+17,p->profile_color);
        memcpy(payload+21,p->icon_rgba+offset,512);
        if(send_frame(peer,W_ICON,0,payload,sizeof(payload)))peer->icon_offsets[i]+=512;
        return;
    }
}
static bool receive_icon(Peer *peer,const uint8_t *b,size_t n) {
    if(n!=21+512)return false;
    uint64_t id=get64(b);
    uint32_t version=get32(b+8),offset=get32(b+12);
    if((get32(b+17)>>24)!=255||!version||offset>MP_ICON_BYTES-512||offset%512)return false;
    int index=-1;
    for(unsigned i=0;i<state.player_count;i++)if(state.players[i].id==id)index=(int)i;
    if(index<0||(state.is_host&&peer->player!=index))return false;
    if(!offset) {
        peer->rx_icon_id=id;
        peer->rx_icon_version=version;
        peer->rx_icon_offset=0;
        peer->rx_icon_color=b[16];
        peer->rx_profile_color=get32(b+17);
    }
    if(peer->rx_icon_id!=id||peer->rx_icon_version!=version||peer->rx_icon_offset!=offset||peer->rx_icon_color!=b[16]||peer->rx_profile_color!=get32(b+17))return false;
    memcpy(peer->rx_icon+offset,b+21,512);
    peer->rx_icon_offset+=512;
    if(peer->rx_icon_offset==MP_ICON_BYTES) {
        MpPlayer *p=&state.players[index];
        if(version>p->icon_revision) {
            p->icon_revision=version;
            p->color_id=peer->rx_icon_color;
            p->profile_color=peer->rx_profile_color;
            memcpy(p->icon_rgba,peer->rx_icon,MP_ICON_BYTES);
            if(state.is_host)broadcast_state();
            else dirty=true;
        }
    }
    return true;
}
static bool receive_message(Peer *peer,int type,uint64_t match,uint32_t seq,const uint8_t *b,size_t n) {
    if(type==W_PING)return n==0;
    /* Appearance is session data, not match data: a queued chunk may cross START. */
    if(type==W_ICON)return receive_icon(peer,b,n);
    if(type==W_RACE_BEGIN||type==W_RACE_PUZZLE||type==W_RACE_END)return receive_race(type,match,b,n);
    if(type==W_ERROR&&!state.is_host) {
        if(!n||n>=sizeof(state.error))return false;
        char error[96];
        memcpy(error,b,n);
        error[n]=0;
        fail(error);
        return true;
    }
    if(state.is_host) {
        if(type==W_HELLO) {
            if(n!=54||(get32(b+50)>>24)!=255||get32(b+41)>MP_MAX_PUZZLES||peer->player>=0||b[40])return false;
            uint64_t id=get64(b);if(!id||id==own_id)return false;
            int idx=-1;
            for(unsigned i=0;i<state.player_count;i++)if(state.players[i].id==id)idx=(int)i;
            bool returning=idx>=0;
            if(!returning&&(state.state!=MP_LOBBY||state.player_count>=MP_MAX_PLAYERS)) {
                const char *error=state.state==MP_LOBBY?"Lobby full":"Match in progress; rejoin only";
                send_frame(peer,W_ERROR,0,(const uint8_t *)error,strlen(error));
                send(peer->fd,peer->tx,peer->tn,0);close_peer(peer);return true;
            }
            if(returning) {
                for(unsigned i=0;i<MP_MAX_PLAYERS-1;i++)if(&peers[i]!=peer&&peers[i].player==idx)close_peer(&peers[i]);
            } else idx=(int)state.player_count++;
            peer->player=idx;MpPlayer *p=&state.players[idx];
            if(!returning)memset(p,0,sizeof(*p));
            p->id=id;p->connected=true;p->ready=false;
            score_sequences[idx]=0;
            p->puzzle_count=get32(b+41);p->solved_count=get32(b+45);p->color_id=b[49];p->profile_color=get32(b+50);
            for(unsigned j=0;j<32&&b[8+j];j++)p->name[j]=b[8+j]>=32?(char)b[8+j]:'?';
            p->name[32]=0;
            cap_rules();broadcast_state();return true;
        }
        if(peer->player<0)return false;
        // Reliable packets can still belong to a match that has just ended.
        if(match!=state.match_id)
            return (type==W_SCORE&&n==12)||(type==W_READY&&n==5)||(type==W_PROGRESS&&n==4);
        if(type==W_SOLVED&&n==4) {
            state.players[peer->player].solved_count=get32(b);
            broadcast_state();
            return true;
        }
        if(type==W_COUNT&&n==4) {
            uint32_t count=get32(b);
            if(count>MP_MAX_PUZZLES)return false;
            state.players[peer->player].puzzle_count=count;
            if(state.state==MP_LOBBY)cap_rules();
            broadcast_state();
            return true;
        }
        if(type==W_READY&&n==5&&(state.state==MP_PREPARING||state.state==MP_STARTING||state.state==MP_PLAYING)) {
            if(!b[0]||!peer->race_end_sent||get32(b+1)!=cached_race_count)return false;
            state.players[peer->player].ready=true;broadcast_state();maybe_start();return true;
        }
        if(type==W_READY&&n==5)return true;
        if(type==W_PROGRESS&&n==4) {
            if(state.state!=MP_PLAYING&&state.state!=MP_STARTING)return true;
            MpPlayer *p=&state.players[peer->player];uint32_t index=get32(b);
            if(index<p->puzzle_index||index>p->puzzle_index+1||index>1000000)return false;
            p->puzzle_index=index;dirty=true;return true;
        }
        if(type==W_SCORE&&n==12) {
            score(peer->player,seq,get32(b),get64(b+4));
            return true;
        }
        return false;
    }
    if(type!=W_STATE||n<16||b[0]>MP_RESULTS||b[1]<1||b[1]>MP_MAX_PLAYERS||n!=16+(size_t)b[1]*76)return false;
    MpRules rules= {
        b[3],get32(b+4),get32(b+8)
    };
    if(!valid_rules(rules))return false;
    memcpy(previous_players,state.players,sizeof(previous_players));
    unsigned previous_count=state.player_count;
    MpState previous=state.state;
    uint64_t previous_match=state.match_id;
    if(previous_match!=match)clear_race();
    state.state=b[0];
    state.player_count=b[1];
    state.winner_mask=b[2];
    state.rules=rules;
    state.match_id=match;
    size_t off=16;
    bool found=false;
    for(unsigned i=0;i<state.player_count;i++) {
        MpPlayer *p=&state.players[i];
        uint64_t id=get64(b+off);
        memset(p,0,sizeof(*p));
        for(unsigned j=0;j<previous_count;j++) if(previous_players[j].id==id) {
            p->icon_revision=previous_players[j].icon_revision;
            memcpy(p->icon_rgba,previous_players[j].icon_rgba,MP_ICON_BYTES);
            break;
        }
        p->id=id;
        if(id==own_id) {
            p->icon_revision=local_icon_revision;
            memcpy(p->icon_rgba,local_icon,MP_ICON_BYTES);
        }
        p->score=get32(b+off+8);
        p->elapsed_us=get64(b+off+12);
        p->connected=b[off+20]!=0;
        p->ready=b[off+21]!=0;
        if(b[off+54])return false;
        memcpy(p->name,b+off+22,33);
        if(p->id==own_id)found=true;
        p->puzzle_count=get32(b+off+55);
        if(p->puzzle_count>MP_MAX_PUZZLES)return false;
        p->solved_count=get32(b+off+59);
        p->color_id=b[off+63];
        p->profile_color=get32(b+off+64);
        if((p->profile_color>>24)!=255)return false;
        p->puzzle_index=get32(b+off+72);
        if(p->puzzle_index>1000000)return false;
        off+=76;
    }
    if(!found)return false;
    /* Loading may finish while HELLO is already in flight. Advertise that count
       once the host has admitted us, rather than waiting for a UI retry. */
    if(previous==MP_CONNECTING) {
        uint8_t count[4];
        put32(count,local_puzzle_count);
        if(!send_frame(peer,W_COUNT,0,count,4))return false;
        put32(count,local_solved_count);
        if(!send_frame(peer,W_SOLVED,0,count,4))return false;
    }
    if(state.state==MP_STARTING||state.state==MP_PLAYING||state.state==MP_FINALIZING||state.state==MP_RESULTS) {
        if(previous==MP_CONNECTING||previous_match!=match||previous!=state.state) {
            int32_t timing=(int32_t)get32(b+12);
            if(timing>=0)state.start_ticks=ticks_after((uint32_t)timing);
            else {
                uint64_t elapsed=(uint64_t)(-(int64_t)timing)*SDL_GetPerformanceFrequency()/1000;
                uint64_t now=SDL_GetPerformanceCounter();state.start_ticks=now>elapsed?now-elapsed:1;
            }
        }
    } else state.start_ticks=0;
    dirty=true;
    return true;
}
static void process_command(Command c) {
    if(c.type==C_LOCAL||c.type==C_WIFI) {
        transport_stop();
        clear_race();
        memset(&state,0,sizeof(state));
        memset(score_sequences,0,sizeof(score_sequences));
        own_sequence=0;
        state.own_id=own_id;
        state.players[0].puzzle_count=local_puzzle_count;
        state.players[0].solved_count=local_solved_count;
        state.players[0].color_id=local_color;
        state.players[0].profile_color=local_profile_color;
        state.players[0].icon_revision=local_icon_revision;
        memcpy(state.players[0].icon_rgba,local_icon,MP_ICON_BYTES);
        state.rules=(MpRules) {
            MP_TIMED,300,10
        };
        snprintf(state.players[0].name,33,"%s",c.name[0]?c.name:"PLAYER");
        const IMultiplayerTransport *transport=c.type==C_LOCAL?&LocalWirelessTransport:&WifiTransport;
        if(!c.port&&c.type==C_WIFI) {
            fail("Invalid port");
            return;
        }
        if(c.type==C_WIFI&&!c.host) {
            struct in_addr addr;
            if(inet_pton(AF_INET,c.ip,&addr)!=1) {
                fail("Invalid IP");
                return;
            }
        }
        if(!transport->begin(&c))fail(c.type==C_LOCAL?"Local discovery unavailable":"Unable to host or connect");
    }
    else if(c.type==C_LOCAL_JOIN) {local_join(c.elapsed);}
    else if(c.type==C_LIBRARY) {
        free(host_library);host_library=c.library;host_library_count=c.score;
        process_command((Command){.type=C_COUNT,.score=c.score});
    }
    else if(c.type==C_APPEARANCE) {
        local_color=c.color_id;
        local_profile_color=c.profile_color;
        memcpy(local_icon,c.icon_rgba,MP_ICON_BYTES);
        if(!++local_icon_revision)++local_icon_revision;
        for(unsigned i=0;i<state.player_count;i++) if(state.players[i].id==own_id) {
            state.players[i].color_id=local_color;
            state.players[i].profile_color=local_profile_color;
            state.players[i].icon_revision=local_icon_revision;
            memcpy(state.players[i].icon_rgba,local_icon,MP_ICON_BYTES);
        }
        if(state.is_host)broadcast_state();
        else dirty=true;
    }
    else if(c.type==C_STOP) {
        transport_stop();
        clear_race();
        state.state=MP_INACTIVE;
        dirty=true;
    }
    else if(c.type==C_RULES&&state.is_host&&state.state==MP_LOBBY&&valid_rules(c.rules)) {
        state.rules=c.rules;
        cap_rules();
        broadcast_state();
    }
    else if(c.type==C_COUNT) {
        local_puzzle_count=c.score;
        for(unsigned i=0;i<state.player_count;i++) if(state.players[i].id==own_id)state.players[i].puzzle_count=c.score;
        if(state.is_host&&state.state==MP_LOBBY) {
            cap_rules();
            broadcast_state();
        }
        else if(!state.is_host&&state.player_count&&state.state>=MP_LOBBY&&state.state<=MP_RESULTS) {
            uint8_t b[4];
            put32(b,c.score);
            if(!send_frame(&peers[0],W_COUNT,0,b,4))fail("Network send queue full");
        }
    }
    else if(c.type==C_SOLVED) {
        local_solved_count=c.score;
        for(unsigned i=0;i<state.player_count;i++) if(state.players[i].id==own_id)state.players[i].solved_count=c.score;
        if(state.is_host&&state.state==MP_LOBBY)broadcast_state();
        else if(!state.is_host&&state.player_count&&state.state>=MP_LOBBY&&state.state<=MP_RESULTS) {
            uint8_t b[4];
            put32(b,c.score);
            if(!send_frame(&peers[0],W_SOLVED,0,b,4))fail("Network send queue full");
        }
    }
    else if(c.type==C_RETURN_LOBBY&&state.is_host&&state.state==MP_RESULTS) {
        for(unsigned i=state.player_count;i>0;) {
            --i;
            if(state.players[i].connected)continue;
            memmove(state.players+i,state.players+i+1,(state.player_count-i-1)*sizeof(MpPlayer));
            --state.player_count;
            for(unsigned j=0;j<MP_MAX_PLAYERS-1;j++)if(peers[j].player>(int)i)--peers[j].player;
        }
        clear_race();
        state.state=MP_LOBBY;
        state.start_ticks=0;
        state.winner_mask=0;
        state.error[0]=0;
        phase_deadline=0;
        own_sequence=0;
        memset(score_sequences,0,sizeof(score_sequences));
        for(unsigned i=0;i<state.player_count;i++) {
            state.players[i].score=0;
            state.players[i].elapsed_us=0;
            state.players[i].ready=false;
            state.players[i].puzzle_index=0;
        }
        cap_rules();
        broadcast_state();
    }
    else if(c.type==C_PREPARE&&state.is_host&&state.state==MP_LOBBY) {
        if(!puzzle_limit())return;
        cap_rules();
        state.match_id=SDL_GetPerformanceCounter()^own_id;
        if(!state.match_id)state.match_id=1;
        state.state=MP_PREPARING;
        own_sequence=0;
        phase_deadline=now_ms()+PREPARE_MS;
        memset(score_sequences,0,sizeof(score_sequences));
        for(unsigned i=0;i<state.player_count;i++) {
            state.players[i].score=0;
            state.players[i].elapsed_us=0;
            state.players[i].ready=false;
            state.players[i].puzzle_index=0;
        }
        clear_race();
        if(!prepare_host_race()) {fail("Unable to prepare host puzzle sequence");return;}
        state.players[0].ready=true;
        broadcast_state();maybe_start();
    }
    else if(c.type==C_READY&&(state.state==MP_PREPARING||state.state==MP_STARTING||state.state==MP_PLAYING)) {
        if(!c.ready||!state.race_ready)return;
        if(state.is_host) {state.players[0].ready=true;broadcast_state();maybe_start();}
        else {
            uint8_t b[5];b[0]=1;put32(b+1,cached_race_count);
            if(!send_frame(&peers[0],W_READY,0,b,5))fail("Network send queue full");
        }
    }
    else if(c.type==C_PROGRESS&&(state.state==MP_STARTING||state.state==MP_PLAYING)) {
        if(c.score>1000000)return;
        for(unsigned i=0;i<state.player_count;i++)if(state.players[i].id==own_id)state.players[i].puzzle_index=c.score;
        if(!state.is_host) {uint8_t b[4];put32(b,c.score);if(!send_frame(&peers[0],W_PROGRESS,0,b,4))fail("Network send queue full");}
        dirty=true;
    }
    else if(c.type==C_SCORE&&(state.state==MP_PLAYING||state.state==MP_FINALIZING)) {
        ++own_sequence;
        if(state.is_host)score(0,own_sequence,c.score,c.elapsed);
        else {
            uint8_t b[12];
            put32(b,c.score);
            put64(b+4,c.elapsed);
            if(!send_frame(&peers[0],W_SCORE,own_sequence,b,12))fail("Network send queue full");
        }
    }
}
static void accept_peer(void) {
    int fd=accept(listener,NULL,NULL);
    if(fd<0)return;
    nonblock(fd);
    for(unsigned i=0;i<MP_MAX_PLAYERS-1;i++)if(peers[i].fd<0) {
        peers[i].fd=fd;
        peers[i].last_rx=peers[i].joined_at=now_ms();
        return;
    }
    /* Best effort small rejection; worker owns this nonblocking socket. */
    uint8_t rejected[30]= {
        'C','N','R','1',PROTOCOL_VERSION,W_ERROR,0,10
    };
    memcpy(rejected+20,"Lobby full",10);
    send(fd,rejected,sizeof(rejected),0);
    close(fd);
}
static void service_peer(Peer *p,bool readable,bool writable) {
    if(p->connecting&&writable) {
        int error=0;
        socklen_t size=sizeof(error);
        getsockopt(p->fd,SOL_SOCKET,SO_ERROR,&error,&size);
        if(error) {
            lost_peer(p);
            return;
        }
        p->connecting=false;
        uint8_t b[54];
        put64(b,own_id);
        memcpy(b+8,state.players[0].name,33);
        put32(b+41,local_puzzle_count);
        put32(b+45,local_solved_count);
        b[49]=local_color;
        put32(b+50,local_profile_color);
        send_frame(p,W_HELLO,0,b,sizeof(b));
    }
    if(writable&&!p->connecting&&p->tn>p->sent) {
        ssize_t n=send(p->fd,p->tx+p->sent,p->tn-p->sent,0);
        if(n>0) {
            p->sent+=n;
            if(p->sent==p->tn)p->sent=p->tn=0;
        }
        else if(n<0&&errno!=EWOULDBLOCK&&errno!=EAGAIN&&errno!=EINTR) {
            lost_peer(p);
            return;
        }
    }
    if(!readable||p->connecting)return;
    ssize_t n=recv(p->fd,p->rx+p->rn,sizeof(p->rx)-p->rn,0);
    if(n==0||(n<0&&errno!=EWOULDBLOCK&&errno!=EAGAIN&&errno!=EINTR)) {
        lost_peer(p);
        return;
    }
    if(n<0)return;
    p->rn+=n;
    p->last_rx=now_ms();
    while(p->rn>=20) {
        size_t len=(size_t)p->rx[6]<<8|p->rx[7];
        if(memcmp(p->rx,"CNR1",4)||p->rx[4]!=PROTOCOL_VERSION||len>FRAME_MAX-20) {
            if(!state.is_host) fail("Protocol incompatible");
            else {
                uint8_t rejected[41]= {
                    'C','N','R','1',PROTOCOL_VERSION,W_ERROR,0,21
                };
                memcpy(rejected+20,"Protocol incompatible",21);
                send(p->fd,rejected,sizeof(rejected),0);
                lost_peer(p);
            }
            return;
        }
        if(p->rn<20+len)break;
        if(!receive_message(p,p->rx[5],get64(p->rx+8),get32(p->rx+16),p->rx+20,len)) {
            lost_peer(p);
            return;
        }
        if(p->fd<0)return;
        p->rn-=20+len;
        memmove(p->rx,p->rx+20+len,p->rn);
    }
}
static void deadlines(void) {
    uint64_t now=now_ms();
    if(state.state==MP_CONNECTING&&now>connect_deadline) {
        fail("Connection timed out");
        return;
    }
    if(state.state==MP_PREPARING&&state.is_host&&now>phase_deadline) {
        for(unsigned i=0;i<MP_MAX_PLAYERS-1;i++)if(peers[i].player>=0&&!state.players[peers[i].player].ready)lost_peer(&peers[i]);
        if(!state.players[0].ready) {
            fail("Puzzle preparation timed out");
            return;
        }
        maybe_start();
    }
    if(state.state==MP_STARTING&&SDL_GetPerformanceCounter()>=state.start_ticks) {
        state.state=MP_PLAYING;
        dirty=true;
    }
    if(state.is_host&&state.state==MP_PLAYING&&state.rules.mode==MP_TIMED&&SDL_GetPerformanceCounter()-state.start_ticks>=(uint64_t)state.rules.duration_seconds*SDL_GetPerformanceFrequency()) {
        state.state=MP_FINALIZING;
        phase_deadline=now+FINAL_MS;
        broadcast_state();
    }
    if(state.is_host&&state.state==MP_FINALIZING&&now>=phase_deadline)finish();
    for(unsigned i=0;i<MP_MAX_PLAYERS-1;i++)if(peers[i].fd>=0&&
    (now-peers[i].last_rx>DEAD_MS ||
    (state.is_host&&peers[i].player<0&&now-peers[i].joined_at>5000))) lost_peer(&peers[i]);
    if(now>=next_ping) {
        next_ping=now+2000;
        for(unsigned i=0;i<MP_MAX_PLAYERS-1;i++)if(peers[i].fd>=0&&!peers[i].connecting&&!send_frame(&peers[i],W_PING,0,NULL,0))lost_peer(&peers[i]);
    }
}
static int worker(void *unused) {
    (void)unused;
    uint64_t scan_at=0;
    while(!atomic_load(&quit)) {
#if CONNECTIONS_PROFILE
        uint64_t profile_began=platform_profile_is_enabled()?SDL_GetPerformanceCounter():0;
#endif
        Command c;
        while(dequeue(&c))process_command(c);
        if(local_mode&&state.state==MP_DISCOVERING&&now_ms()>=scan_at) {
            scan_at=now_ms()+750;
            local_discover();
        }
        if(local_mode&&local_host&&now_ms()>=next_advertise) {next_advertise=now_ms()+1000;local_advertise();}
        deadlines();
        publish();
        fd_set r,w;
        FD_ZERO(&r);
        FD_ZERO(&w);
        int maxfd=-1;
        if(listener>=0) {
            FD_SET(listener,&r);
            maxfd=listener;
        }
        for(unsigned i=0;i<MP_MAX_PLAYERS-1;i++) {
            Peer *p=&peers[i];
            if(p->fd>=0) {
                queue_state(p);
                queue_race(p);
                queue_icon_chunk(p);
                FD_SET(p->fd,&r);
                if(p->connecting||p->tn>p->sent)FD_SET(p->fd,&w);
                if(p->fd>maxfd)maxfd=p->fd;
            }
        }
#if CONNECTIONS_PROFILE
        if(profile_began) platform_profile_worker_sample((uint32_t)((SDL_GetPerformanceCounter()-profile_began)*1000000/SDL_GetPerformanceFrequency()));
#endif
        if(maxfd<0) {
            SDL_Delay(25);
            continue;
        }
        struct timeval wait= {
            0,25000
        };
        int result=select(maxfd+1,&r,&w,NULL,&wait);
        if(result<=0)continue;
#if CONNECTIONS_PROFILE
        profile_began=platform_profile_is_enabled()?SDL_GetPerformanceCounter():0;
#endif
        if(listener>=0&&FD_ISSET(listener,&r))accept_peer();
        for(unsigned i=0;i<MP_MAX_PLAYERS-1;i++) {
            Peer *p=&peers[i];
            if(p->fd>=0)service_peer(p,FD_ISSET(p->fd,&r),FD_ISSET(p->fd,&w));
        }
#if CONNECTIONS_PROFILE
        if(profile_began) platform_profile_worker_sample((uint32_t)((SDL_GetPerformanceCounter()-profile_began)*1000000/SDL_GetPerformanceFrequency()));
#endif
    }
    transport_stop();
    clear_race();
    free(host_library);host_library=NULL;host_library_count=0;
    return 0;
}
bool mp_init(void) {
    if(thread)return true;
    queue_lock=SDL_CreateMutex();
    snapshot_lock=SDL_CreateMutex();
    race_lock=SDL_CreateMutex();
    if(!queue_lock||!snapshot_lock||!race_lock) {
        if(queue_lock)SDL_DestroyMutex(queue_lock);
        if(snapshot_lock)SDL_DestroyMutex(snapshot_lock);
        if(race_lock)SDL_DestroyMutex(race_lock);
        queue_lock=snapshot_lock=race_lock=NULL;
        return false;
    }
    memset(&published,0,sizeof(published));
    memset(&state,0,sizeof(state));
    qread=qcount=0;
    read_revision=0;
    atomic_store(&revision,0);
    atomic_store(&quit,false);
    own_id=SDL_GetPerformanceCounter();
    randomGet(&own_id,sizeof(own_id));
    if(!own_id)own_id=1;
    for(unsigned i=0;i<MP_MAX_PLAYERS-1;i++) {
        peers[i].fd=-1;
        peers[i].player=-1;
    }
    thread=SDL_CreateThread(worker,"Connections network",NULL);
    if(!thread) {
        SDL_DestroyMutex(queue_lock);
        SDL_DestroyMutex(snapshot_lock);
        SDL_DestroyMutex(race_lock);
        queue_lock=snapshot_lock=race_lock=NULL;
        return false;
    }
    return true;
}
void mp_shutdown(void) {
    if(!thread)return;
    atomic_store(&quit,true);
    SDL_WaitThread(thread,NULL);
    thread=NULL;
    Command pending;
    while(dequeue(&pending))if(pending.type==C_LIBRARY)free(pending.library);
    SDL_DestroyMutex(queue_lock);SDL_DestroyMutex(snapshot_lock);SDL_DestroyMutex(race_lock);
    queue_lock=snapshot_lock=race_lock=NULL;
}
bool mp_begin_local(const char *name) {return mp_discover_local(name);}
bool mp_begin_local_host(const char *name) {
    Command c={.type=C_LOCAL,.host=true};snprintf(c.name,sizeof(c.name),"%s",name?name:"PLAYER");return enqueue(c);
}
bool mp_discover_local(const char *name) {
    Command c={.type=C_LOCAL};snprintf(c.name,sizeof(c.name),"%s",name?name:"PLAYER");return enqueue(c);
}
bool mp_join_local(uint64_t session_id) {return session_id&&enqueue((Command){.type=C_LOCAL_JOIN,.elapsed=session_id});}
bool mp_begin_wifi(bool host,const char *ip,uint16_t port,const char *name) {
    Command c= {
        .type=C_WIFI,.host=host,.port=port
    };
    snprintf(c.ip,sizeof(c.ip),"%s",ip?ip:"");
    snprintf(c.name,sizeof(c.name),"%s",name?name:"PLAYER");
    return enqueue(c);
}
void mp_stop(void) {
    if(!thread)return;
    ConnectionsPuzzle *discarded[QUEUE_MAX];unsigned count=0;
    SDL_LockMutex(queue_lock);
    for(unsigned i=0;i<qcount;i++) {
        Command *c=&queue[(qread+i)%QUEUE_MAX];
        if(c->type==C_LIBRARY&&c->library)discarded[count++]=c->library;
    }
    qread=0;qcount=1;queue[0]=(Command){.type=C_STOP};
    SDL_UnlockMutex(queue_lock);
    for(unsigned i=0;i<count;i++)free(discarded[i]);
}
bool mp_set_rules(MpRules rules) {
    return enqueue((Command) {
        .type=C_RULES,.rules=rules
    });
}
bool mp_set_puzzle_count(unsigned count) {
    if(count>MP_MAX_PUZZLES)return false;
    return enqueue((Command) { .type=C_COUNT,.score=count });
}
bool mp_set_library(ConnectionsPuzzle *puzzles,unsigned count,unsigned entries) {
    if(count>MP_MAX_PUZZLES||entries<count||(!puzzles&&count)||(!count&&puzzles))return false;
    return enqueue((Command){.type=C_LIBRARY,.library=puzzles,.score=count});
}
static uint32_t race_random(uint32_t *seed) {
    *seed^=*seed<<13;
    *seed^=*seed>>17;
    *seed^=*seed<<5;
    return *seed;
}
bool mp_race_puzzle(uint64_t match_id,unsigned index,ConnectionsPuzzle *out) {
    if(!thread||!out||!match_id)return false;
    SDL_LockMutex(race_lock);
    bool ready=cached_race&&cached_race_count&&cached_race_match==match_id;
    if(ready)*out=cached_race[index%cached_race_count];
    SDL_UnlockMutex(race_lock);return ready;
}
bool mp_set_progress(unsigned index) {return index<=1000000&&enqueue((Command){.type=C_PROGRESS,.score=index});}
bool mp_return_lobby(void) {
    return enqueue((Command){.type=C_RETURN_LOBBY});
}
bool mp_set_solved_count(uint32_t count) {
    return enqueue((Command) { .type=C_SOLVED,.score=count });
}
bool mp_set_appearance(uint8_t color_id,uint32_t profile_color,const uint8_t rgba[MP_ICON_BYTES]) {
    if(!rgba||(profile_color>>24)!=255)return false;
    Command c={ .type=C_APPEARANCE,.color_id=color_id,.profile_color=profile_color };
    memcpy(c.icon_rgba,rgba,MP_ICON_BYTES);
    return enqueue(c);
}
bool mp_prepare_match(void) {
    return enqueue((Command) {
        .type=C_PREPARE
    });
}
bool mp_ready(bool usable) {
    return enqueue((Command) {
        .type=C_READY,.ready=usable
    });
}
bool mp_complete(uint32_t score_value,uint64_t elapsed_us) {
    return enqueue((Command) {
        .type=C_SCORE,.score=score_value,.elapsed=elapsed_us
    });
}
bool mp_read_snapshot(MpSnapshot *snapshot) {
    unsigned r=atomic_load_explicit(&revision,memory_order_acquire);
    if(!snapshot||!thread||r==read_revision)return false;
    SDL_LockMutex(snapshot_lock);
    *snapshot=published;
    SDL_UnlockMutex(snapshot_lock);
    read_revision=r;
    return true;
}
