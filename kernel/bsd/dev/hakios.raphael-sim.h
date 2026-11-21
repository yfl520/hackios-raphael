/* Raphael UIM control on the already-running MPSS/Wi-Fi QRTR transport.
 * Wire formats: libqmi qmi-service-{uim,dms,nas}.json. No second DSP or thread.
 * Provisioning follows HakiOS a48d2b2; identifiers are never written to the log.
 * RF/data bring-up is separate: this client does not start WDS or program IPA.
 */
#include <sys/hakios.nas.h>
#include <sys/sysctl.h>
#define RAPHAEL_SIM_CARDS 8
#define RAPHAEL_SIM_APPS 16
struct raphael_sim_app { uint8_t type, state, pin1, retries; };
struct raphael_sim_card {
	uint8_t state, upin, error, apps;
	struct raphael_sim_app app[RAPHAEL_SIM_APPS];
};
struct raphael_sim_status {
	uint16_t primary;
	uint8_t cards;
	struct raphael_sim_card card[RAPHAEL_SIM_CARDS];
	uint8_t primary_aid_slot, primary_aid_size, primary_aid[32];
};
struct raphael_slot_status {
	uint8_t slots;
	struct { uint32_t card, active; uint8_t logical; } slot[RAPHAEL_SIM_CARDS];
};
static uint16_t raphael_sim_u16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t raphael_sim_u32(const uint8_t *p) {
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
#include "hakios.raphael-operator.h"

/* Publish nothing until every variable-length application has passed bounds. */
static int
raphael_sim_decode(const uint8_t *v, size_t n, struct raphael_sim_status *out)
{
	struct raphael_sim_status s = {0}; size_t at = 9;
	if (n < at || v[8] > RAPHAEL_SIM_CARDS) return EPROTO;
	s.primary = raphael_sim_u16(v); s.cards = v[8];
	for (unsigned c = 0; c < s.cards; c++) {
		if (n - at < 6) return EPROTO;
		struct raphael_sim_card *card = &s.card[c];
		card->state=v[at]; card->upin=v[at+1]; card->error=v[at+4]; card->apps=v[at+5]; at+=6;
		if (card->apps > RAPHAEL_SIM_APPS) return EPROTO;
		for (unsigned a=0; a<card->apps; a++) {
			if (n-at<7) return EPROTO;
			size_t aid=v[at+6]; if (n-at<14+aid) return EPROTO;
			struct raphael_sim_app *app=&card->app[a];
			app->type=v[at]; app->state=v[at+1]; app->pin1=v[at+8+aid]; app->retries=v[at+9+aid];
			if (card->state==1 && (app->type==1 || app->type==2) && (app->state==1 || app->state==7) &&
			    aid && aid<=sizeof(s.primary_aid) && !s.primary_aid_size) {
				s.primary_aid_slot=(uint8_t)(c+1);s.primary_aid_size=(uint8_t)aid;memcpy(s.primary_aid,v+at+7,aid);
			}
			at+=14+aid;
		}
	}
	if (at!=n) return EPROTO; *out=s; return 0;
}
static int
raphael_slots_decode(const uint8_t *v, size_t n, struct raphael_slot_status *out)
{
	struct raphael_slot_status s={0}; size_t at=1;
	if (!n || v[0]>RAPHAEL_SIM_CARDS) return EPROTO; s.slots=v[0];
	for (unsigned i=0;i<s.slots;i++) {
		if (n-at<10) return EPROTO;
		s.slot[i].card=raphael_sim_u32(v+at); s.slot[i].active=raphael_sim_u32(v+at+4); s.slot[i].logical=v[at+8];
		size_t iccid=v[at+9]; if (n-at-10<iccid) return EPROTO;
		at+=10+iccid; /* skip ICCID, never copy or log it */
	}
	if (at!=n) return EPROTO; *out=s; return 0;
}

static int raphael_sim_state, raphael_sim_slot, raphael_modem_mode=-1;
SYSCTL_DECL(_kern_hakios);
SYSCTL_INT(_kern_hakios, OID_AUTO, sim_state, CTLFLAG_RD|CTLFLAG_LOCKED, &raphael_sim_state,0,
    "0 unknown, 1 absent, 2 detected, 3 PIN/PUK required, 4 ready, 5 card error");
SYSCTL_INT(_kern_hakios, OID_AUTO, sim_slot, CTLFLAG_RD|CTLFLAG_LOCKED, &raphael_sim_slot,0,"selected logical SIM slot, 0 unknown");
SYSCTL_INT(_kern_hakios, OID_AUTO, modem_mode, CTLFLAG_RD|CTLFLAG_LOCKED, &raphael_modem_mode,0,"QMI DMS operating mode, -1 unknown");
extern void hakios_raphael_cellular_commit(bool up, const struct hakios_nas_state *state);
struct raphael_sim_client {
	struct hakios_qrtr_port *local;
	uint32_t service,node,remote,instance;
	uint16_t message,transaction;
	uint64_t deadline;
	bool pending,responded;
};
static struct raphael_sim_client raphael_uim,raphael_dms,raphael_operator;
static struct hakios_nas_client raphael_nas;
static struct raphael_sim_status raphael_cards;
static struct raphael_slot_status raphael_slots;
static bool raphael_sim_started,raphael_sim_finished,raphael_card_valid;
static bool raphael_events_tried,raphael_switch_tried,raphael_provision_tried,raphael_lowpower_tried;
static unsigned raphael_power_tried,raphael_nas_answers;
static uint64_t raphael_sim_began,raphael_sim_poll,raphael_sim_report;
static unsigned raphael_sim_round;

static struct raphael_home_network raphael_home;
static char raphael_provider[64];
static int raphael_provider_source; /* 0 unknown, 1 NAS SPN, 2 card EF_SPN */
static uint32_t raphael_spn_generation=1,raphael_spn_requested_generation;
static uint16_t raphael_spn_requested_primary;
static unsigned raphael_spn_attempts;
static uint64_t raphael_spn_poll;
static int raphael_home_mcc,raphael_home_mnc,raphael_home_digits;
static uint64_t raphael_home_poll;
SYSCTL_INT(_kern_hakios,OID_AUTO,sim_home_mcc,CTLFLAG_RD|CTLFLAG_LOCKED,&raphael_home_mcc,0,"home PLMN MCC, 0 unknown; not serving network");
SYSCTL_INT(_kern_hakios,OID_AUTO,sim_home_mnc,CTLFLAG_RD|CTLFLAG_LOCKED,&raphael_home_mnc,0,"home PLMN MNC, see sim_home_mcc for validity");
SYSCTL_INT(_kern_hakios,OID_AUTO,sim_home_mnc_digits,CTLFLAG_RD|CTLFLAG_LOCKED,&raphael_home_digits,0,"0 unknown, 2 or 3 MNC digits");
SYSCTL_STRING(_kern_hakios,OID_AUTO,sim_home_name,CTLFLAG_RD|CTLFLAG_LOCKED,raphael_home.name,sizeof(raphael_home.name),"modem-reported home operator description; not registration evidence");
SYSCTL_STRING(_kern_hakios,OID_AUTO,sim_provider_name,CTLFLAG_RD|CTLFLAG_LOCKED,raphael_provider,sizeof(raphael_provider),"SIM service provider from card EF_SPN or NAS SPN; not network registration");
SYSCTL_INT(_kern_hakios,OID_AUTO,sim_provider_source,CTLFLAG_RD|CTLFLAG_LOCKED,&raphael_provider_source,0,"0 unknown, 1 NAS SPN, 2 card EF_SPN; not serving network");
static void
raphael_home_clear(void)
{
    bzero(&raphael_home,sizeof(raphael_home));raphael_home_mcc=raphael_home_mnc=raphael_home_digits=0;
    raphael_home_poll=0;raphael_operator.pending=false;raphael_operator.message=0x39;
    bzero(raphael_provider,sizeof(raphael_provider));raphael_provider_source=0;
    if(!++raphael_spn_generation)++raphael_spn_generation;
    raphael_spn_requested_generation=0;raphael_spn_attempts=0;raphael_spn_poll=0;
    if(raphael_uim.pending && raphael_uim.message==0x20)raphael_uim.pending=false;
}

/* Use the actual primary app index, never just the first card's first app. */
static uint8_t
raphael_spn_app_type(void)
{
    unsigned card=raphael_cards.primary>>8,app=raphael_cards.primary&255;
    if(!raphael_card_valid || raphael_sim_state!=4 || raphael_sim_slot<1 ||
        (raphael_modem_mode!=0 && raphael_modem_mode!=1) ||
        card!=(unsigned)(raphael_sim_slot-1) || card>=raphael_cards.cards ||
        raphael_cards.card[card].state!=1 || app>=raphael_cards.card[card].apps)return 0;
    const struct raphael_sim_app *a=&raphael_cards.card[card].app[app];
    return a->state==7 && (a->type==1 || a->type==2)?a->type:0;
}
static int
raphael_sim_request(struct raphael_sim_client *q,uint16_t msg,const uint8_t *body,size_t n)
{
	uint8_t frame[80]; if (!q->local || !q->remote || q->pending || n>sizeof(frame)-7) return EBUSY;
	if (++q->transaction==0) ++q->transaction;
	frame[0]=0;frame[1]=q->transaction;frame[2]=q->transaction>>8;frame[3]=msg;frame[4]=msg>>8;frame[5]=n;frame[6]=n>>8;
	if (n) memcpy(frame+7,body,n);
	q->message=msg;q->pending=true;q->deadline=now_us()+5000000ull;
	int error=qrtr_send(q->local,q->node,q->remote,frame,n+7);
	if (error) q->pending=false;
	return error;
}
static void
raphael_sim_publish(void)
{
	int state=1,slot=0;
	for(unsigned c=0;c<raphael_cards.cards;c++) {
		const struct raphael_sim_card *card=&raphael_cards.card[c];
		int value=card->state==2?5:card->state==1?2:1;
		for(unsigned a=0;card->state==1 && a<card->apps;a++) {
			const struct raphael_sim_app *app=&card->app[a];
			if(app->type!=1 && app->type!=2) continue;
			if(app->state==7) value=4;
			else if(app->state==2 || app->state==3 || app->state==5) value=3;
		}
		/* Prefer a present card over an errored empty slot. */
		if((value>=2 && value<=4 && (state==1 || state==5 || value>state)) || (state==1 && value==5)) {state=value;slot=c+1;}
	}
	if(state!=raphael_sim_state || slot!=raphael_sim_slot)
		printf("raphael-sim: state=%d logical-slot=%d primary-GW=%#x\n",state,slot,raphael_cards.primary);
	if(state!=raphael_sim_state || slot!=raphael_sim_slot)raphael_home_clear();
	raphael_sim_state=state;raphael_sim_slot=slot;
}
static void
raphael_sim_receive(void *context,uint32_t node,uint32_t remote,const void *data,size_t n)
{
	struct raphael_sim_client *q=context; const uint8_t *m=data,*value=NULL,*result=NULL;
	size_t value_n=0,at=7; uint16_t msg;
	if(!q->local || node!=q->node || remote!=q->remote || n<7 || raphael_sim_u16(m+5)!=n-7) return;
	msg=raphael_sim_u16(m+3);
	bool indication=m[0]==4 && q==&raphael_uim && (msg==0x32 || msg==0x48);
	if(!indication && (m[0]!=2 || !q->pending || msg!=q->message || raphael_sim_u16(m+1)!=q->transaction)) return;
    if(q==&raphael_uim && msg==0x20) {
        q->pending=false;q->responded=true;raphael_spn_poll=now_us()+30000000ull;
        if(raphael_spn_requested_generation!=raphael_spn_generation ||
            raphael_spn_requested_primary!=raphael_cards.primary || !raphael_spn_app_type()) {
            printf("raphael-sim: stale EF_SPN reply discarded\n");return;
        }
        char provider[64]={0};int error=raphael_spn_decode(m,n,provider);
        if(error) {
            uint16_t code=0;(void)raphael_operator_result(m,n,0x20,&code);
            printf("raphael-sim: EF_SPN read rejected=%d QMI-error=%u attempt=%u\n",error,code,raphael_spn_attempts);return;
        }
        memcpy(raphael_provider,provider,sizeof(provider));raphael_provider_source=2;
        printf("raphael-sim: SIM service-provider=%s (card EF_SPN; not serving registration)\n",provider);
        return;
    }
	if(q==&raphael_operator) {
        if(msg!=0x25 && msg!=0x39)return;
        q->pending=false;q->responded=true;
        /* Try the other name query promptly; successful home/SPN lasts 5 min. */
        raphael_home_poll=now_us()+((msg==0x25 && !raphael_provider[0])?1000000ull:30000000ull);
        if(msg==0x39) {
            char provider[64]={0};int error=raphael_provider_decode(m,n,provider);
            if(error){raphael_operator_rejected(m,n,msg,error);return;}
            if(strcmp(provider,raphael_provider))printf("raphael-sim: SIM service-provider=%s (NAS SPN; not serving registration)\n",provider);
            if(raphael_provider_source!=2){memcpy(raphael_provider,provider,sizeof(provider));raphael_provider_source=1;}
            if(raphael_home_mcc)raphael_home_poll=now_us()+300000000ull;
            return;
        }
        struct raphael_home_network home;int error=raphael_home_decode(m,n,&home);
        if(error){raphael_operator_rejected(m,n,msg,error);return;}
        if(memcmp(&home,&raphael_home,sizeof(home)))
            printf("raphael-sim: home MCC=%u MNC=%u digits=%u name=%s (not serving registration)\n",home.mcc,home.mnc,home.mnc_digits,home.name);
        raphael_home=home;raphael_home_mcc=home.mcc;raphael_home_mnc=home.mnc;raphael_home_digits=home.mnc_digits;
        if(raphael_provider[0])raphael_home_poll=now_us()+300000000ull;
        return;
    }
	while(at<n) {
		if(n-at<3) return; uint8_t type=m[at];size_t len=raphael_sim_u16(m+at+1);at+=3;
		if(len>n-at) return;
		if(type==2) {if(result || len!=4) return;result=m+at;}
		if(type==(q==&raphael_uim?0x10:1)) {if(value) return;value=m+at;value_n=len;}
		at+=len;
	}
	if(!indication) {
		if(!result) return; q->pending=false;q->responded=true;
		if(raphael_sim_u16(result)) {
			printf("raphael-sim: service=%#x msg=%#x QMI-error=%u\n",q->service,msg,raphael_sim_u16(result+2));return;
		}
	}
	if(q==&raphael_dms) {
		if(msg==0x2d && value && value_n==1) {
			if(raphael_modem_mode!=value[0]) printf("raphael-sim: modem-mode=%u (0 online,1 low-power,3 offline)\n",value[0]);
			raphael_modem_mode=value[0];
		} else if(msg==0x2e) printf("raphael-sim: low-power transition acknowledged; RF not requested\n");
	} else if((msg==0x2f || msg==0x32) && value) {
		struct raphael_sim_status status;
		if(raphael_sim_decode(value,value_n,&status)) {printf("raphael-sim: malformed card status rejected\n");return;}
		bool changed=!raphael_card_valid || memcmp(&status,&raphael_cards,sizeof(status));
        /* Indications also invalidate same-shaped replacement cards. No identifiers needed. */
        if(changed || indication)raphael_home_clear();
		raphael_cards=status;raphael_card_valid=true;raphael_sim_publish();
		if(changed) for(unsigned c=0;c<status.cards;c++) {
			const struct raphael_sim_card *card=&status.card[c];
			printf("raphael-sim: logical-slot=%u card=%u error=%u applications=%u\n",c+1,card->state,card->error,card->apps);
			for(unsigned a=0;a<card->apps;a++) printf("raphael-sim: logical-slot=%u app=%u type=%u state=%u PIN1=%u\n",c+1,a,card->app[a].type,card->app[a].state,card->app[a].pin1);
		}
	} else if((msg==0x47 || msg==0x48) && value) {
		struct raphael_slot_status status;
		if(raphael_slots_decode(value,value_n,&status)) {printf("raphael-sim: malformed physical slots rejected\n");return;}
		if(memcmp(&status,&raphael_slots,sizeof(status))) for(unsigned i=0;i<status.slots;i++)
			printf("raphael-sim: physical-slot=%u card=%u active=%u logical=%u\n",i+1,status.slot[i].card,status.slot[i].active,status.slot[i].logical);
		raphael_slots=status;
	} else if(msg==0x31 || msg==0x38 || msg==0x46 || msg==0x2e) {
		printf("raphael-sim: UIM action=%#x acknowledged; waiting for fresh card status\n",msg);
	}
}
static void
raphael_sim_server(void *context,bool arrived,uint32_t service,uint32_t version,uint32_t instance,uint32_t node,uint32_t remote)
{
	struct raphael_sim_client *q=context;(void)version;
	if(q->service!=service) return;
	if(!arrived) {
		if(q->node==node && q->remote==remote) {
			q->remote=0;q->pending=false;q->responded=false;
			if(q==&raphael_uim) {raphael_card_valid=false;raphael_sim_state=raphael_sim_slot=0;raphael_home_clear();}
			if(q==&raphael_operator)raphael_home_clear();
		}
		return;
	}
	/* Never let a later service instance replace an active endpoint. */
	if(q->remote) return;
	q->node=node;q->remote=remote;q->instance=instance;
	printf("raphael-sim: service=%#x version=%u instance=%u at %u:%u\n",service,version,instance,node,remote);
}
static void
raphael_nas_changed(__unused void *context,const struct hakios_nas_state *state)
{
	hakios_raphael_cellular_commit(raphael_nas.answers!=0,state);
	printf("raphael-sim: network registered=%u radio=%u dBm=%d bars=%u\n",state->registered,state->radio_interface,state->dbm,state->bars);
}
static void
raphael_sim_close(void)
{
	if(!raphael_sim_started || raphael_sim_finished) return;
	if(raphael_uim.local) qrtr_close(raphael_uim.local);
	if(raphael_dms.local) qrtr_close(raphael_dms.local);
	if(raphael_operator.local)qrtr_close(raphael_operator.local);
	raphael_operator.local=NULL;raphael_home_clear();
	raphael_uim.local=raphael_dms.local=NULL;
	hakios_nas_client_close(&raphael_nas);hakios_raphael_cellular_commit(false,NULL);
	bzero(&raphael_cards,sizeof(raphael_cards));raphael_sim_state=raphael_sim_slot=0;
	raphael_sim_finished=true;
}
/* All sends and callbacks use Wi-Fi's single QRTR dispatcher; never sleep here. */
static void
raphael_sim_tick(void)
{
	if(raphael_sim_finished) return;uint64_t now=now_us();
	if(!raphael_sim_started) {
		if(!glink.open) return;
		if(!hakios_is_raphael() || hakios_wifi_option("uim_off")==1) {raphael_sim_finished=true;return;}
		raphael_sim_started=true;raphael_sim_began=now;raphael_uim.service=0xb;raphael_dms.service=2;raphael_operator.service=3;
		struct raphael_sim_client *clients[]={&raphael_uim,&raphael_dms,&raphael_operator};
		for(unsigned i=0;i<3;i++) {
			struct raphael_sim_client *q=clients[i];q->transaction=0x7300+i*0x100;
			q->local=qrtr_open(raphael_sim_receive,raphael_sim_server,q);
			if(q->local) qrtr_lookup(q->local,q->service,0,0);
		}
		hakios_nas_client_open(&raphael_nas,&qrtr_ops,raphael_nas_changed,NULL);
		printf("raphael-sim: persistent UIM/DMS/NAS client on existing MPSS; RF/data off; NV remains RAM-only\n");
	}
	struct raphael_sim_client *clients[]={&raphael_uim,&raphael_dms,&raphael_operator};
	for(unsigned i=0;i<3;i++) if(clients[i]->pending && now>=clients[i]->deadline) {
		printf("raphael-sim: service=%#x msg=%#x timed out\n",clients[i]->service,clients[i]->message);clients[i]->pending=false;
	}
	if(raphael_nas.answers!=raphael_nas_answers) {
		raphael_nas_answers=raphael_nas.answers;hakios_raphael_cellular_commit(true,&raphael_nas.state);
	}
	if(now<raphael_sim_poll) return;raphael_sim_poll=now+1000000ull;
	if(!raphael_dms.pending && raphael_dms.remote) {
		if((raphael_modem_mode==3 || raphael_modem_mode==5) && !raphael_lowpower_tried && now-raphael_sim_began>20000000ull) {
			const uint8_t body[]={1,1,0,1};raphael_lowpower_tried=true;
			printf("raphael-sim: requesting OFFLINE -> LOW POWER for UIM initialization\n");
			raphael_sim_request(&raphael_dms,0x2e,body,sizeof(body));
		} else raphael_sim_request(&raphael_dms,0x2d,NULL,0);
	}
    /* Same NAS endpoint/default primary GW as the serving-state client.
     * Separate local QRTR port avoids stealing its outstanding responses.
     * Never blocks UIM status/provisioning and never changes radio power. */
    if((raphael_modem_mode==0 || raphael_modem_mode==1) && raphael_sim_state==4 && raphael_card_valid && raphael_cards.primary!=0xffff &&
        (raphael_cards.primary>>8)==(unsigned)(raphael_sim_slot-1) &&
        raphael_nas.connected && raphael_operator.remote && !raphael_operator.pending &&
        raphael_operator.node==raphael_nas.peer_node && raphael_operator.remote==raphael_nas.peer_port &&
        now>=raphael_home_poll) {
        raphael_home_poll=now+30000000ull;
        uint16_t msg=raphael_operator.message==0x25?0x39:0x25;
        raphael_sim_request(&raphael_operator,msg,NULL,0);
    }
	if(raphael_nas.connected && !(raphael_sim_round%3)) hakios_nas_client_poll(&raphael_nas);
	if(raphael_uim.remote && !raphael_uim.pending) {
		if(!raphael_events_tried) {
			const uint8_t body[]={1,4,0,1,0,0,0};raphael_events_tried=true;
			raphael_sim_request(&raphael_uim,0x2e,body,sizeof(body));
		} else if(raphael_card_valid && raphael_sim_state==1 && now-raphael_sim_began>12000000ull &&
		    raphael_power_tried<raphael_cards.cards && raphael_power_tried<2) {
			const uint8_t slot=(uint8_t)(++raphael_power_tried),body[]={1,1,0,slot};
			printf("raphael-sim: request SIM power-on logical-slot=%u (once)\n",slot);
			raphael_sim_request(&raphael_uim,0x31,body,sizeof(body));
		} else if(raphael_card_valid && raphael_cards.primary==0xffff && raphael_cards.cards && !raphael_provision_tried) {
			if(raphael_cards.primary_aid_size) {
				uint8_t size=raphael_cards.primary_aid_size;
				uint8_t body[42]={1,2,0,0,1,0x10,(uint8_t)(2+size),0,raphael_cards.primary_aid_slot,size};
				memcpy(body+10,raphael_cards.primary_aid,size);raphael_provision_tried=true;
				printf("raphael-sim: activating primary GW session from detected logical-slot=%u application\n",raphael_cards.primary_aid_slot);
				raphael_sim_request(&raphael_uim,0x38,body,10+size);
			} else raphael_sim_request(&raphael_uim,0x2f,NULL,0);
		} else if(hakios_wifi_option("uim_switch")==1 && !raphael_switch_tried && raphael_card_valid && raphael_cards.cards && raphael_cards.card[0].state!=1 &&
		    raphael_sim_state>=2 && raphael_sim_state<=4 && raphael_sim_slot>1) {
			/* Use the physical mapping only when the firmware has actually reported it. */
			unsigned physical=0;
			for(unsigned i=0;i<raphael_slots.slots;i++) if(raphael_slots.slot[i].logical==raphael_sim_slot && raphael_slots.slot[i].card==2) physical=i+1;
			if(physical) {
				uint8_t body[]={1,1,0,1,2,4,0,(uint8_t)physical,0,0,0};raphael_switch_tried=true;
				printf("raphael-sim: map primary logical slot to detected physical-slot=%u\n",physical);
				raphael_sim_request(&raphael_uim,0x46,body,sizeof(body));
			} else raphael_sim_request(&raphael_uim,(raphael_sim_round%5)?0x2f:0x47,NULL,0);
        } else if(raphael_spn_app_type() && raphael_provider_source!=2 &&
            raphael_spn_attempts<3 && now>=raphael_spn_poll && raphael_sim_round%5==2) {
            uint8_t body[22];size_t bytes=raphael_spn_request_body(raphael_spn_app_type(),body);
            raphael_spn_requested_generation=raphael_spn_generation;
            raphael_spn_requested_primary=raphael_cards.primary;
            raphael_spn_poll=now+30000000ull;raphael_spn_attempts++;
            printf("raphael-sim: read EF_SPN on primary-GW logical-slot=%d attempt=%u\n",raphael_sim_slot,raphael_spn_attempts);
            raphael_sim_request(&raphael_uim,0x20,body,bytes);
        } else raphael_sim_request(&raphael_uim,(raphael_sim_round%5)?0x2f:0x47,NULL,0);
	}
	raphael_sim_round++;
	if(now-raphael_sim_report>30000000ull) {
		raphael_sim_report=now;
		printf("raphael-sim: summary mode=%d state=%d slot=%d primary=%#x NAS-answers=%u elapsed=%llu s\n",raphael_modem_mode,
		    raphael_sim_state,raphael_sim_slot,raphael_cards.primary,raphael_nas.answers,(now-raphael_sim_began)/1000000ull);
	}
}
