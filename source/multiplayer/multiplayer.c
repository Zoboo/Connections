#include "multiplayer.h"
#include "platform/platform_profile.h"
#include <SDL2/SDL.h>
#include <switch.h>
#include <stdatomic.h>
#include <stdio.h>
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
enum {
    W_HELLO=1, W_STATE, W_READY, W_SCORE, W_PING, W_ERROR, W_COUNT, W_SOLVED, W_ICON
};
enum {
    C_LOCAL=1, C_WIFI, C_STOP, C_RULES, C_PREPARE, C_READY, C_SCORE, C_COUNT, C_SOLVED, C_APPEARANCE, C_RETURN_LOBBY
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
}
Command;
typedef struct {
    int fd, player;
    bool connecting;
    uint8_t rx[FRAME_MAX], tx[FRAME_MAX*4];
    size_t rn, tn, sent;
    uint64_t last_rx, joined_at;
    uint64_t icon_ids[MP_MAX_PLAYERS], rx_icon_id;
    uint32_t icon_versions[MP_MAX_PLAYERS], icon_offsets[MP_MAX_PLAYERS];
    uint32_t rx_icon_version, rx_icon_offset, rx_profile_color;
    uint8_t rx_icon_color, rx_icon[MP_ICON_BYTES];
}
Peer;
typedef struct {
    bool (*begin)(const Command *);
    void (*stop)(void);
}
IMultiplayerTransport;
static SDL_Thread *thread;
static SDL_mutex *queue_lock, *snapshot_lock;
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
/* Main-thread cache: workers never dereference the caller-owned library. */
static const ConnectionsPuzzle *race_library;
static unsigned race_library_count;
static uint64_t race_sequence_match;
static uint32_t race_order[MP_MAX_PUZZLES];
static uint8_t race_word_order[MP_MAX_PUZZLES][CONNECTIONS_WORD_COUNT];
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
    b[4]=4;
    b[5]=type;
    b[6]=len>>8;
    b[7]=len;
    put64(b+8,state.match_id);
    put32(b+16,seq);
    if(len)memcpy(b+20,payload,len);
    p->tn+=20+len;
    return true;
}
static void broadcast_state(void) {
    /* Remove slow peers before encoding. This avoids emitting an old membership
       snapshot after a newer disconnect snapshot to the remaining clients. */
    for(unsigned i=0;i<MP_MAX_PLAYERS-1;i++) {
        Peer *p=&peers[i];
        size_t frame_size=20+16+state.player_count*72;
        if(p->fd>=0&&p->player>=0&&!p->connecting&&p->tn-p->sent+frame_size>sizeof(p->tx)) drop_peer(p);
    }
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
    uint64_t countdown=0,now=SDL_GetPerformanceCounter();
    if(state.start_ticks>now)countdown=(state.start_ticks-now)*1000/SDL_GetPerformanceFrequency();
    put32(b+n,countdown);
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
    }
    for(unsigned i=0;i<MP_MAX_PLAYERS-1;i++)if(peers[i].fd>=0&&peers[i].player>=0&&!peers[i].connecting) {
        send_frame(&peers[i],W_STATE,0,b,n);
    }
    dirty=true;
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
        else state.players[idx].connected=false;
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
static bool local_begin(const Command *c) {
    (void)c;
    local_mode=true;
    if(R_FAILED(ldnInitialize(LdnServiceType_User))) return false;
    ldn_up=true;
    if(R_FAILED(ldnOpenStation())) return false;
    state.state=MP_DISCOVERING;
    dirty=true;
    connect_deadline=now_ms()+1200+(own_id%1800);
    return true;
}
static const IMultiplayerTransport WifiTransport= {
    wifi_begin,transport_stop
};
static const IMultiplayerTransport LocalWirelessTransport= {
    local_begin,transport_stop
};
static void local_discover(void) {
    LdnScanFilter filter= {
        0
    };
    filter.flags=LdnScanFilterFlag_SceneId;
    filter.network_id.intent_id.scene_id=LOCAL_SCENE;
    LdnNetworkInfo nets[8]= {
        0
    };
    s32 count=0;
    if(R_FAILED(ldnScan(0,&filter,nets,8,&count))) {
        fail("Local discovery unavailable");
        return;
    }
    LdnSecurityConfig sec= {
        0
    };
    sec.security_mode=LdnSecurityMode_Product;
    sec.passphrase_size=16;
    memcpy(sec.passphrase,"ConnectionsRace1",16);
    LdnUserConfig user= {
        0
    };
    memcpy(user.user_name,state.players[0].name,33);
    for(int i=0;i<count;i++)if(nets[i].advertise_data_size>=4&&!memcmp(nets[i].advertise_data,"CNR1",4)) {
        if(R_FAILED(ldnConnect(&sec,&user,1,0,&nets[i])))continue;
        struct in_addr addr= {
            htonl(nets[i].nodes[0].ip_addr.addr)
        };
        char ip[48];
        snprintf(ip,sizeof(ip),"%s",inet_ntoa(addr));
        if(!setup_socket(false,ip,LOCAL_PORT)) fail("Unable to connect locally");
        return;
    }
    if(now_ms()<connect_deadline)return;
    LdnNetworkConfig config= {
        0
    };
    config.intent_id.local_communication_id=-1;
    config.intent_id.scene_id=LOCAL_SCENE;
    config.node_count_max=MP_MAX_PLAYERS;
    config.local_communication_version=1;
    if(R_FAILED(ldnCloseStation())||R_FAILED(ldnOpenAccessPoint())||R_FAILED(ldnCreateNetwork(&sec,&user,&config))||R_FAILED(ldnSetAdvertiseData("CNR1",4))||!setup_socket(true,"",LOCAL_PORT)) {
        fail("Unable to host local wireless");
        return;
    }
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
            if(n!=54||(get32(b+50)>>24)!=255||get32(b+41)>MP_MAX_PUZZLES||peer->player>=0||state.state!=MP_LOBBY||state.player_count>=MP_MAX_PLAYERS)return false;
            uint64_t id=get64(b);
            if(!id)return false;
            for(unsigned i=0;i<state.player_count;i++)if(state.players[i].id==id)return false;
            if(b[40]!=0)return false;
            int idx=state.player_count++;
            peer->player=idx;
            MpPlayer *p=&state.players[idx];
            memset(p,0,sizeof(*p));
            p->id=id;
            p->connected=true;
            p->puzzle_count=get32(b+41);
            p->solved_count=get32(b+45);
            p->color_id=b[49];
            p->profile_color=get32(b+50);
            for(unsigned j=0;j<32&&b[8+j];j++)p->name[j]=b[8+j]>=32?(char)b[8+j]:'?';
            p->name[32]=0;
            cap_rules();
            broadcast_state();
            return true;
        }
        if(peer->player<0)return false;
        // Reliable packets can still belong to a match that has just ended.
        if(match!=state.match_id)
            return (type==W_SCORE&&n==12)||(type==W_READY&&n==5);
        if(type==W_SOLVED&&n==4&&(state.state==MP_LOBBY||state.state==MP_PREPARING)) {
            state.players[peer->player].solved_count=get32(b);
            broadcast_state();
            return true;
        }
        if(type==W_COUNT&&n==4&&(state.state==MP_LOBBY||state.state==MP_PREPARING)) {
            uint32_t count=get32(b);
            if(count>MP_MAX_PUZZLES)return false;
            state.players[peer->player].puzzle_count=count;
            if(state.state==MP_LOBBY)cap_rules();
            broadcast_state();
            return true;
        }
        if(type==W_READY&&n==5&&state.state==MP_PREPARING) {
            uint32_t count=get32(b+1);
            if(count>MP_MAX_PUZZLES)return false;
            if(!b[0]||!count||(state.rules.mode==MP_TARGET_SCORE&&count<state.rules.target_score)) {
                lost_peer(peer);
                return true;
            }
            state.players[peer->player].puzzle_count=count;
            state.players[peer->player].ready=true;
            broadcast_state();
            maybe_start();
            return true;
        }
        if(type==W_SCORE&&n==12) {
            score(peer->player,seq,get32(b),get64(b+4));
            return true;
        }
        return false;
    }
    if(type!=W_STATE||n<16||b[0]>MP_RESULTS||b[1]<1||b[1]>MP_MAX_PLAYERS||n!=16+(size_t)b[1]*72)return false;
    MpRules rules= {
        b[3],get32(b+4),get32(b+8)
    };
    if(!valid_rules(rules))return false;
    memcpy(previous_players,state.players,sizeof(previous_players));
    unsigned previous_count=state.player_count;
    MpState previous=state.state;
    uint64_t previous_match=state.match_id;
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
        off+=72;
    }
    if(!found)return false;
    /* Loading may finish while HELLO is already in flight. Advertise that count
       once the host has admitted us, rather than waiting for a UI retry. */
    if(previous==MP_CONNECTING&&state.state==MP_LOBBY) {
        uint8_t count[4];
        put32(count,local_puzzle_count);
        if(!send_frame(peer,W_COUNT,0,count,4))return false;
        put32(count,local_solved_count);
        if(!send_frame(peer,W_SOLVED,0,count,4))return false;
    }
    if(state.state==MP_STARTING&&(previous!=MP_STARTING||previous_match!=match))state.start_ticks=ticks_after(get32(b+12));
    dirty=true;
    return true;
}
static void process_command(Command c) {
    if(c.type==C_LOCAL||c.type==C_WIFI) {
        transport_stop();
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
        else if(!state.is_host&&(state.state==MP_LOBBY||state.state==MP_PREPARING)) {
            uint8_t b[4];
            put32(b,c.score);
            if(!send_frame(&peers[0],W_COUNT,0,b,4))fail("Network send queue full");
        }
    }
    else if(c.type==C_SOLVED) {
        local_solved_count=c.score;
        for(unsigned i=0;i<state.player_count;i++) if(state.players[i].id==own_id)state.players[i].solved_count=c.score;
        if(state.is_host&&state.state==MP_LOBBY)broadcast_state();
        else if(!state.is_host&&(state.state==MP_LOBBY||state.state==MP_PREPARING)) {
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
        }
        cap_rules();
        broadcast_state();
    }
    else if(c.type==C_PREPARE&&state.is_host&&state.state==MP_LOBBY) {
        if(!puzzle_limit())return;
        cap_rules();
        state.match_id=SDL_GetPerformanceCounter()^own_id;
        state.state=MP_PREPARING;
        own_sequence=0;
        phase_deadline=now_ms()+15000;
        memset(score_sequences,0,sizeof(score_sequences));
        for(unsigned i=0;i<state.player_count;i++) {
            state.players[i].score=0;
            state.players[i].elapsed_us=0;
            state.players[i].ready=false;
        }
        broadcast_state();
    }
    else if(c.type==C_READY&&state.state==MP_PREPARING) {
        if(state.is_host) {
            if(!c.ready||!local_puzzle_count||(state.rules.mode==MP_TARGET_SCORE&&local_puzzle_count<state.rules.target_score)) {
                fail("No usable local puzzles");
                return;
            }
            state.players[0].ready=true;
            broadcast_state();
            maybe_start();
        }
        else {
            uint8_t b[5];
            b[0]=c.ready;
            put32(b+1,local_puzzle_count);
            if(!send_frame(&peers[0],W_READY,0,b,5))fail("Network send queue full");
        }
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
    for(unsigned i=0;i<MP_MAX_PLAYERS-1;i++)if(peers[i].fd<0&&state.state==MP_LOBBY) {
        peers[i].fd=fd;
        peers[i].last_rx=peers[i].joined_at=now_ms();
        return;
    }
    /* Best effort small rejection; worker owns this nonblocking socket. */
    uint8_t rejected[30]= {
        'C','N','R','1',4,W_ERROR,0,10
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
        if(memcmp(p->rx,"CNR1",4)||p->rx[4]!=4||len>FRAME_MAX-20) {
            if(!state.is_host) fail("Protocol incompatible");
            else {
                uint8_t rejected[41]= {
                    'C','N','R','1',4,W_ERROR,0,21
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
    return 0;
}
bool mp_init(void) {
    if(thread)return true;
    queue_lock=SDL_CreateMutex();
    snapshot_lock=SDL_CreateMutex();
    if(!queue_lock||!snapshot_lock) {
        if(queue_lock)SDL_DestroyMutex(queue_lock);
        if(snapshot_lock)SDL_DestroyMutex(snapshot_lock);
        queue_lock=snapshot_lock=NULL;
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
        queue_lock=snapshot_lock=NULL;
        return false;
    }
    return true;
}
void mp_shutdown(void) {
    if(!thread)return;
    atomic_store(&quit,true);
    SDL_WaitThread(thread,NULL);
    thread=NULL;
    SDL_DestroyMutex(queue_lock);
    SDL_DestroyMutex(snapshot_lock);
    queue_lock=snapshot_lock=NULL;
    race_library=NULL;
    race_library_count=0;
    race_sequence_match=0;
}
bool mp_begin_local(const char *name) {
    Command c= {
        .type=C_LOCAL
    };
    snprintf(c.name,sizeof(c.name),"%s",name?name:"PLAYER");
    return enqueue(c);
}
bool mp_begin_wifi(bool host,const char *ip,uint16_t port,const char *name) {
    Command c= {
        .type=C_WIFI,.host=host,.port=port
    };
    snprintf(c.ip,sizeof(c.ip),"%s",ip?ip:"");
    snprintf(c.name,sizeof(c.name),"%s",name?name:"PLAYER");
    return enqueue(c);
}
void mp_stop(void) {
    Command c= {
        .type=C_STOP
    };
    if(!thread)return;
    SDL_LockMutex(queue_lock);
    qread=0;
    qcount=1;
    queue[0]=c;
    SDL_UnlockMutex(queue_lock);
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
bool mp_set_library(const ConnectionsPuzzle *puzzles,unsigned count,unsigned entries) {
    if(count>MP_MAX_PUZZLES||entries<count||(!puzzles&&count)||(!count&&puzzles))return false;
    race_library=puzzles;
    race_library_count=count;
    race_sequence_match=0;
    return mp_set_puzzle_count(count);
}
static uint32_t race_random(uint32_t *seed) {
    *seed^=*seed<<13;
    *seed^=*seed>>17;
    *seed^=*seed<<5;
    return *seed;
}
bool mp_race_puzzle(uint64_t match_id,unsigned index,ConnectionsPuzzle *out) {
    if(!thread||!out||!match_id||!race_library||!race_library_count)return false;
    SDL_LockMutex(snapshot_lock);
    uint64_t current_match=published.match_id;
    MpState current_state=published.state;
    SDL_UnlockMutex(snapshot_lock);
    if(match_id!=current_match)return false;
    if(race_sequence_match!=match_id) {
        if(current_state!=MP_PREPARING)return false;
        uint64_t entropy=SDL_GetPerformanceCounter()^own_id^match_id;
        uint32_t seed=(uint32_t)(entropy^(entropy>>32));
        if(!seed)seed=0x9e3779b9;
        for(unsigned i=0;i<race_library_count;i++) {
            race_order[i]=i;
            for(unsigned j=0;j<CONNECTIONS_WORD_COUNT;j++)race_word_order[i][j]=(uint8_t)j;
            for(unsigned j=CONNECTIONS_WORD_COUNT-1;j>0;j--) {
                unsigned k=race_random(&seed)%(j+1);
                uint8_t swap=race_word_order[i][j];
                race_word_order[i][j]=race_word_order[i][k];
                race_word_order[i][k]=swap;
            }
        }
        for(unsigned i=race_library_count-1;i>0;i--) {
            unsigned j=race_random(&seed)%(i+1);
            uint32_t swap=race_order[i];race_order[i]=race_order[j];race_order[j]=swap;
        }
        race_sequence_match=match_id;
    }
    if(current_state!=MP_PREPARING&&current_state!=MP_STARTING&&current_state!=MP_PLAYING)return false;
    unsigned slot=index%race_library_count;
    unsigned selected=race_order[slot];
    const ConnectionsPuzzle *source=&race_library[selected];
    *out=*source;
    for(unsigned i=0;i<CONNECTIONS_WORD_COUNT;i++) {
        unsigned from=race_word_order[selected][i];
        memcpy(out->words[i],source->words[from],CONNECTIONS_WORD_MAX_LENGTH);
        out->group_ids[i]=source->group_ids[from];
    }
    return true;
}
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
