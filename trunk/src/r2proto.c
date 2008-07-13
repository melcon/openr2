/*
 * OpenR2 
 * MFC/R2 call setup library
 *
 * Moises Silva <moises.silva@gmail.com>
 * Copyright (C) Moises Silva
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/ioctl.h>
#ifdef HAVE_LINUX_ZAPTEL_H
#include <linux/zaptel.h>
#elif HAVE_ZAPTEL_ZAPTEL_H
#include <zaptel/zaptel.h>
#else
#error "wtf? either linux/zaptel.h or zaptel/zaptel.h should be present"
#endif
#include "openr2/r2chan.h"
#include "openr2/r2log.h"
#include "openr2/r2proto.h"
#include "openr2/r2utils.h"

#define OR2_PROTO_ANSWER_WAIT_TIME 150

#define GA_TONE(r2chan) (r2chan)->r2context->mf_ga_tones
#define GB_TONE(r2chan) (r2chan)->r2context->mf_gb_tones
#define GC_TONE(r2chan) (r2chan)->r2context->mf_gc_tones

#define GI_TONE(r2chan) (r2chan)->r2context->mf_g1_tones
#define GII_TONE(r2chan) (r2chan)->r2context->mf_g2_tones

#define TIMER(r2chan) (r2chan)->r2context->timers

/* Note that we compare >= because even if max_dnis is zero
   we could get 1 digit, want it or not :-) */
#define DNIS_COMPLETE(r2chan) ((r2chan)->dnis_len >= (r2chan)->r2context->max_dnis)

static void r2config_argentina(openr2_context_t *r2context)
{
	OR2_CONTEXT_STACK;
	r2context->mf_g1_tones.no_more_dnis_available = OR2_MF_TONE_INVALID;
	r2context->mf_g1_tones.caller_ani_is_restricted = OR2_MF_TONE_12;
	r2context->timers.r2_metering_pulse = 400;
}

static void r2config_brazil(openr2_context_t *r2context)
{
	OR2_CONTEXT_STACK;
	r2context->mf_g1_tones.no_more_dnis_available = OR2_MF_TONE_INVALID;
	r2context->mf_g1_tones.caller_ani_is_restricted = OR2_MF_TONE_12;

	r2context->mf_ga_tones.address_complete_charge_setup = OR2_MF_TONE_INVALID;

	r2context->mf_gb_tones.accept_call_with_charge = OR2_MF_TONE_1;
	r2context->mf_gb_tones.busy_number = OR2_MF_TONE_2;
	r2context->mf_gb_tones.accept_call_no_charge = OR2_MF_TONE_5;
	r2context->mf_gb_tones.special_info_tone = OR2_MF_TONE_6;
	r2context->mf_gb_tones.unallocated_number = OR2_MF_TONE_7;
}

static void r2config_china(openr2_context_t *r2context)
{
	OR2_CONTEXT_STACK;
	/* In the ITU line signaling specifications, the C and D bits are set to 0 and
	   1 respectively, in China they are both set to 1. However, they are never
	   used, so their value never changes during a call */
	r2context->abcd_nonr2_bits = 0x3;    /* 0011 */

	r2context->mf_ga_tones.request_next_ani_digit = OR2_MF_TONE_1;
	r2context->mf_ga_tones.request_category = OR2_MF_TONE_6;
	r2context->mf_ga_tones.address_complete_charge_setup = OR2_MF_TONE_INVALID;

	r2context->mf_gb_tones.accept_call_with_charge = OR2_MF_TONE_1;
	r2context->mf_gb_tones.busy_number = OR2_MF_TONE_2;
	r2context->mf_gb_tones.special_info_tone = OR2_MF_TONE_INVALID;

	r2context->mf_g1_tones.no_more_dnis_available = OR2_MF_TONE_INVALID;
}

static void r2config_itu(openr2_context_t *r2context)
{
	OR2_CONTEXT_STACK;
	return;
}

static void r2config_mexico(openr2_context_t *r2context)
{
	OR2_CONTEXT_STACK;

	/* Telmex, Avantel and most telcos in Mexico send DNIS first and ANI at the end, however
	   this can be modified by the user because I know of at least 1 telco (Maxcom)
	   which requires the ANI first and the DNIS later */
	r2context->get_ani_first = 0;

	/* Mexico use a special signal to request 
	   calling party category AND switch to Group C */
	r2context->mf_ga_tones.request_category = OR2_MF_TONE_INVALID;
	r2context->mf_ga_tones.request_category_and_change_to_gc = OR2_MF_TONE_6;
	r2context->mf_ga_tones.address_complete_charge_setup = OR2_MF_TONE_INVALID;

	/* GA next ANI is replaces by GC next ANI signal */
	r2context->mf_ga_tones.request_next_ani_digit = OR2_MF_TONE_INVALID;

	/* Group B */
	r2context->mf_gb_tones.accept_call_with_charge = OR2_MF_TONE_1;
	r2context->mf_gb_tones.accept_call_no_charge = OR2_MF_TONE_5;
	r2context->mf_gb_tones.busy_number = OR2_MF_TONE_2;
	r2context->mf_gb_tones.unallocated_number = OR2_MF_TONE_2;
	r2context->mf_gb_tones.special_info_tone = OR2_MF_TONE_INVALID;

	/* GROUP C */
	r2context->mf_gc_tones.request_next_ani_digit = OR2_MF_TONE_1;
	r2context->mf_gc_tones.request_change_to_g2 = OR2_MF_TONE_3;
	r2context->mf_gc_tones.request_next_dnis_digit_and_change_to_ga = OR2_MF_TONE_5;
	
	/* Mexico has no signal when running out of DNIS, 
	   timeout is used instead*/
	r2context->mf_g1_tones.no_more_dnis_available = OR2_MF_TONE_INVALID;
}

static const openr2_abcd_signal_t standard_abcd_signals[OR2_NUM_ABCD_SIGNALS] =
{
	/* OR2_ABCD_IDLE */ 0x8,
	/* OR2_ABCD_BLOCK */ 0xC,
	/* OR2_ABCD_SEIZE */ 0x0,
	/* OR2_ABCD_SEIZE_ACK */ 0xC,
	/* OR2_ABCD_CLEAR_BACK */ 0xC,
	/* OR2_ABCD_CLEAR_FORWARD */ 0x8,
	/* OR2_ABCD_ANSWER */ 0x4
};

static const char *abcd_names[OR2_NUM_ABCD_SIGNALS] =
{
	/* OR2_ABCD_IDLE */ "IDLE",
	/* OR2_ABCD_BLOCK */ "BLOCK",
	/* OR2_ABCD_SEIZE */ "SEIZE",
	/* OR2_ABCD_SEIZE_ACK */ "SEIZE_ACK",
	/* OR2_ABCD_CLEAR_BACK */ "CLEAR_BACK",
	/* OR2_ABCD_CLEAR_FORWARD */ "CLEAR_FORWARD",
	/* OR2_ABCD_ANSWER */ "ANSWER" 
};

typedef void (*openr2_variant_config_func)(openr2_context_t *);
typedef struct {
	openr2_variant_t id;
	const char *name;
	openr2_variant_config_func config;
} openr2_variant_entry_t;

static openr2_variant_entry_t r2variants[] =
{
	/* ARGENTINA */ 
	{
		.id = OR2_VAR_ARGENTINA,
		.name = "AR",
		.config = r2config_argentina,
	},	
	/* BRAZIL */ 
	{
		.id = OR2_VAR_BRAZIL,
		.name = "BR",
		.config = r2config_brazil
	},
	/* CHINA */ 
	{
		.id = OR2_VAR_CHINA,
		.name = "CN",
		.config = r2config_china
	},	
	/* CZECH */ 
	{
		.id = OR2_VAR_CZECH,
		.name = "CZ",
		.config = r2config_itu
	},		
	/* ECUADOR */ 
	{
		.id = OR2_VAR_ECUADOR,
		.name = "EC",
		.config = r2config_itu,
	},	
	/* ITU */
	{
		.id = OR2_VAR_ITU,
		.name = "ITU",
		.config = r2config_itu
	},
	/* MEXICO */ 
	{
		.id = OR2_VAR_MEXICO,
		.name = "MX",
		.config = r2config_mexico
	},
	/* PHILIPPINES */ 
	{
		.id = OR2_VAR_PHILIPPINES,
		.name = "PH",
		.config = r2config_itu
	}
};

static int set_abcd_signal(openr2_chan_t *r2chan, openr2_abcd_signal_t signal)
{
	OR2_CHAN_STACK;
	int res, abcd, myerrno;
	abcd = r2chan->r2context->abcd_signals[signal];
	openr2_log(r2chan, OR2_LOG_CAS_TRACE, "ABCD Tx >> [%s] 0x%X\n", abcd_names[signal], abcd);
	r2chan->abcd_write = abcd;
	/* set the NON R2 bits to 1 */
	abcd |= r2chan->r2context->abcd_nonr2_bits; 
	res = ioctl(r2chan->fd, ZT_SETTXBITS, &abcd);
	if (res) {
		myerrno = errno;
		EMI(r2chan)->on_os_error(r2chan, myerrno);
		openr2_log(r2chan, OR2_LOG_ERROR, "ZT_SETTXBITS failed: %s\n", strerror(myerrno));
		return -1;
	} 
	return 0;
}

/* here we configure R2 as ITU and finally call a country specific function to alter the protocol description acording
   to the specified R2 variant */
int openr2_proto_configure_context(openr2_context_t *r2context, openr2_variant_t variant, int max_ani, int max_dnis)
{
	OR2_CONTEXT_STACK;
	unsigned i = 0;
	unsigned limit = sizeof(r2variants)/sizeof(r2variants[0]);
	/* if we don't know that variant, return failure */
	for (i = 0; i < limit; i++) {
		if (variant == r2variants[i].id) {
			break;
		}
	}
	if (i == limit) {
		return -1;
	}

	/* set default standard ABCD signals */
	memcpy(r2context->abcd_signals, standard_abcd_signals, sizeof(standard_abcd_signals));

	/* Default Non-R2 bit required to be on is D */
	r2context->abcd_nonr2_bits = 0x1;    /* 0001 */

	/* Default R2 bits are A and B */
	r2context->abcd_r2_bits = 0xC; /*  1100 */

	/* set default values for the protocol timers */
	r2context->timers.mf_back_cycle = 1500;
	r2context->timers.mf_back_resume_cycle = 150;
	r2context->timers.mf_fwd_safety = 10000;
	r2context->timers.r2_seize = 8000;
	r2context->timers.r2_answer = 80000; 
	r2context->timers.r2_metering_pulse = 0;

	/* Max ANI and DNIS */
	r2context->max_dnis = max_dnis;
	r2context->max_ani = max_ani;

	/* the forward R2 side always send DNIS first but
	   most variants continue by asking ANI first
	   and continuing with DNIS at the end  */
	r2context->get_ani_first = 1;

	/* Group A tones. Requests of ANI, DNIS and Calling Party Category */
	r2context->mf_ga_tones.request_next_dnis_digit = OR2_MF_TONE_1;
	r2context->mf_ga_tones.request_next_ani_digit = OR2_MF_TONE_5;
	r2context->mf_ga_tones.request_category = OR2_MF_TONE_5;
	r2context->mf_ga_tones.request_category_and_change_to_gc = OR2_MF_TONE_INVALID;
	r2context->mf_ga_tones.request_change_to_g2 = OR2_MF_TONE_3;
        /* It's unusual, but an ITU-compliant switch can accept in Group A */
	r2context->mf_ga_tones.address_complete_charge_setup = OR2_MF_TONE_6;
	r2context->mf_ga_tones.network_congestion = OR2_MF_TONE_4;

	/* Group B tones. Decisions about what to do with the call */
	r2context->mf_gb_tones.accept_call_with_charge = OR2_MF_TONE_6;
	r2context->mf_gb_tones.accept_call_no_charge = OR2_MF_TONE_7;
	r2context->mf_gb_tones.busy_number = OR2_MF_TONE_3;
	r2context->mf_gb_tones.network_congestion = OR2_MF_TONE_4;
	r2context->mf_gb_tones.unallocated_number = OR2_MF_TONE_5;
	r2context->mf_gb_tones.line_out_of_order = OR2_MF_TONE_8;
	r2context->mf_gb_tones.special_info_tone = OR2_MF_TONE_2;

	/* Group C tones. Similar to Group A but for Mexico */
	r2context->mf_gc_tones.request_next_ani_digit = OR2_MF_TONE_INVALID;
	r2context->mf_gc_tones.request_change_to_g2 = OR2_MF_TONE_INVALID;
	r2context->mf_gc_tones.request_next_dnis_digit_and_change_to_ga = OR2_MF_TONE_INVALID;

	/* Group I tones. Attend requests of Group A  */
	r2context->mf_g1_tones.no_more_dnis_available = OR2_MF_TONE_15;
	r2context->mf_g1_tones.no_more_ani_available = OR2_MF_TONE_15;
	r2context->mf_g1_tones.caller_ani_is_restricted = OR2_MF_TONE_INVALID;

	/* Group II tones. */
	r2context->mf_g2_tones.national_subscriber = OR2_MF_TONE_1;
	r2context->mf_g2_tones.national_priority_subscriber = OR2_MF_TONE_2;
	r2context->mf_g2_tones.international_subscriber = OR2_MF_TONE_7;
	r2context->mf_g2_tones.international_priority_subscriber = OR2_MF_TONE_9;

	/* now configure the country specific variations */
	r2variants[i].config(r2context);
	return 0;
}

static const char *r2state2str(openr2_abcd_state_t r2state)
{
	switch (r2state) {
	case OR2_IDLE:
		return "Idle";
	case OR2_SEIZE_ACK_TXD:
		return "Seize ACK Transmitted";
	case OR2_ANSWER_TXD:
		return "Answer Transmitted";
	case OR2_CLEAR_BACK_TXD:
		return "Clear Back Transmitted";
	case OR2_CLEAR_FWD_RXD:
		return "Clear Forward Received";
	case OR2_SEIZE_TXD:
		return "Seize Transmitted";
	case OR2_SEIZE_ACK_RXD:
		return "Seize ACK Received";
	case OR2_CLEAR_BACK_TONE_RXD:
		return "Clear Back Tone Received";
	case OR2_ACCEPT_RXD:
		return "Accept Received";
	case OR2_ANSWER_RXD:
		return "Answer Received";
	case OR2_CLEAR_BACK_RXD:
		return "Clear Back Received";
	case OR2_ANSWER_RXD_MF_PENDING:
		return "Answer Received with MF Pending";
	case OR2_CLEAR_FWD_TXD:
		return "Clear Forward Transmitted";
	case OR2_BLOCKED:
		return "Blocked";
	default: 
		return "*Unknown*(%d)";
	}
}

static const char *mfstate2str(openr2_mf_state_t mf_state)
{
	switch (mf_state) {
	case OR2_MF_OFF_STATE:
		return "MF Engine Off";

	case OR2_MF_SEIZE_ACK_TXD:
		return "Seize ACK Transmitted";
	case OR2_MF_CATEGORY_RQ_TXD:
		return "Category Request Transmitted";
	case OR2_MF_DNIS_RQ_TXD:
		return "DNIS Request Transmitted";
	case OR2_MF_ANI_RQ_TXD:
		return "ANI Request Transmitted";
	case OR2_MF_CHG_GII_TXD:
		return "Change To Group II Request Transmitted";
	case OR2_MF_ACCEPTED_TXD:
		return "Accepted Call Transmitted";
	case OR2_MF_DISCONNECT_TXD:
		return "Disconnect Tone Transmitted";

	case OR2_MF_CATEGORY_TXD:
		return "Category Transmitted";
	case OR2_MF_DNIS_TXD:
		return "DNIS Digit Transmitted";
	case OR2_MF_DNIS_END_TXD:
		return "End of DNIS Transmitted";
	case OR2_MF_ANI_TXD:
		return "ANI Digit Transmitted";
	case OR2_MF_ANI_END_TXD:
		return "End of ANI Transmitted";
	case OR2_MF_WAITING_TIMEOUT:
		return "Waiting Far End Timeout";

	default:
		return "*Unknown*";
	}
}

const char *openr2_proto_get_error(openr2_protocol_error_t error)
{
	switch ( error ) {
	case OR2_INVALID_CAS_BITS:
		return "Invalid CAS";
	case OR2_INVALID_MF_TONE:
		return "Invalid Multi Frequency Tone";
	case OR2_BACK_MF_TIMEOUT:
		return "Multi Frequency Cycle Timeout";
	case OR2_SEIZE_TIMEOUT:
		return "Seize Timeout";
	case OR2_ANSWER_TIMEOUT:
		return "Answer Timeout";
	case OR2_INVALID_R2_STATE:
		return "Invalid R2 state";
	case OR2_INVALID_MF_STATE:
		return "Invalid Multy Frequency State";
	case OR2_INVALID_MF_GROUP:
		return "Invalid R2 Group";
	case OR2_FWD_SAFETY_TIMEOUT:
		return "Forward Safety Timeout";
	case OR2_BROKEN_MF_SEQUENCE:
		return "Broken MF Sequence";
	case OR2_LIBRARY_BUG:
		return "OpenR2 Library BUG";
	case OR2_INTERNAL_ERROR:
		return "OpenR2 Internal Error";
	default:
		return "*Unknown*";
	}
}

static const char *mfgroup2str(openr2_mf_group_t mf_group)
{
	switch ( mf_group ) {
	case OR2_MF_NO_GROUP:
		return "No Group";

	case OR2_MF_BACK_INIT:
		return "Backward MF init";
	case OR2_MF_GA:
		return "Backward Group A";
	case OR2_MF_GB:
		return "Backward Group B";
	case OR2_MF_GC:
		return "Backward Group C";

	case OR2_MF_FWD_INIT:
		return "Forward MF init";
	case OR2_MF_GI:
		return "Forward Group I";
	case OR2_MF_GII:
		return "Forward Group II";
	case OR2_MF_GIII:
		return "Forward Group III";

	default:
		return "*Unknown*";
	}
}

static const char *callstate2str(openr2_call_state_t state)
{
	switch (state) {
	case OR2_CALL_IDLE:
		return "Idle";
	case OR2_CALL_DIALING:
		return "Dialing";
	case OR2_CALL_OFFERED:
		return "Offered";
	case OR2_CALL_ACCEPTED:
		return "Accepted";
	case OR2_CALL_ANSWERED:
		return "Answered";
	case OR2_CALL_DISCONNECTED:
		return "Disconnected";
	}	
	return "*Unknown*";
}

const char *openr2_proto_get_disconnect_string(openr2_call_disconnect_cause_t cause)
{
	switch (cause) {
	case OR2_CAUSE_BUSY_NUMBER:
		return "Busy Number";
	case OR2_CAUSE_NETWORK_CONGESTION:
		return "Network Congestion";
	case OR2_CAUSE_UNALLOCATED_NUMBER:
		return "Unallocated Number";
	case OR2_CAUSE_OUT_OF_ORDER:
		return "Line Out Of Order";
	case OR2_CAUSE_UNSPECIFIED:
		return "Not Specified";
	case OR2_CAUSE_NORMAL_CLEARING:
		return "Normal Clearing";
	case OR2_CAUSE_NO_ANSWER:
		return "No Answer";
	default:
		return "*Unknown*";
	}
}

static void openr2_proto_init(openr2_chan_t *r2chan)
{
	/* cancel any event we could be waiting for */
	openr2_chan_cancel_timer(r2chan);

	/* initialize all the proto and call stuff */
	r2chan->ani[0] = '\0';
	r2chan->ani_len = 0;
	r2chan->ani_ptr = NULL;
	r2chan->dnis[0] = '\0';
	r2chan->dnis_len = 0;
	r2chan->dnis_ptr = NULL;
	r2chan->caller_ani_is_restricted = 0;
	r2chan->caller_category = OR2_MF_TONE_INVALID;
	r2chan->r2_state = OR2_IDLE;
	r2chan->mf_state = OR2_MF_OFF_STATE;
	r2chan->mf_group = OR2_MF_NO_GROUP;
	r2chan->call_state = OR2_CALL_IDLE;
	r2chan->direction = OR2_DIR_STOPPED;
	r2chan->answered = 0;
	r2chan->category_sent = 0;
	r2chan->mf_write_tone = 0;
	r2chan->mf_read_tone = 0;
	if (r2chan->logfile) {
		if (fclose(r2chan->logfile)) {
			openr2_log(r2chan, OR2_LOG_WARNING, "Failed to close log file, leaking fds!.\n");
		}
		r2chan->logfile = NULL;
	}

}

int openr2_proto_set_idle(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	openr2_proto_init(r2chan);
	if (set_abcd_signal(r2chan, OR2_ABCD_IDLE)) {
		r2chan->r2context->last_error = OR2_LIBERR_CANNOT_SET_IDLE;
		openr2_log(r2chan, OR2_LOG_ERROR, "failed to set channel %d to IDLE state.\n");
		return -1;
	}
	return 0;
}

int openr2_proto_set_blocked(openr2_chan_t *r2chan)
{
	openr2_proto_init(r2chan);
	r2chan->r2_state = OR2_BLOCKED;
	if (set_abcd_signal(r2chan, OR2_ABCD_BLOCK)) {
		openr2_log(r2chan, OR2_LOG_ERROR, "failed to set channel %d to BLOCKED state.\n");
		return -1;
	}
	return 0;
}

static void handle_protocol_error(openr2_chan_t *r2chan, openr2_protocol_error_t reason)
{
	OR2_CHAN_STACK;
	openr2_log(r2chan, OR2_LOG_ERROR, "Protocol error. Reason = %s, R2 State = %s, MF state = %s, MF Group = %s\n", 
			openr2_proto_get_error(reason), r2state2str(r2chan->r2_state), mfstate2str(r2chan->mf_state), mfgroup2str(r2chan->mf_group));
	openr2_log(r2chan, OR2_LOG_DEBUG, "DNIS = %s, ANI = %s, Last MF Signal = %c\n", r2chan->dnis, r2chan->ani, 
			r2chan->mf_read_tone ? r2chan->mf_read_tone : 0x20);
	/* mute anything we may have */
	MFI(r2chan)->mf_select_tone(r2chan->mf_write_handle, 0);
	openr2_proto_set_idle(r2chan);
	EMI(r2chan)->on_protocol_error(r2chan, reason);
}

static void open_logfile(openr2_chan_t *r2chan, int backward)
{
	time_t currtime;
	char stringbuf[512];
	char currdir[512];
	char timestr[30];
	int res = 0;
	char *cres = NULL;
	int myerrno = 0;
	if (!r2chan->r2context->logdir) {
		cres = getcwd(currdir, sizeof(currdir));
		if (!cres) {
			myerrno = errno;
			EMI(r2chan)->on_os_error(r2chan, myerrno);
			openr2_log(r2chan, OR2_LOG_WARNING, "Could not get cwd: %s\n", strerror(myerrno));
			return;
		}
	}
	res = snprintf(stringbuf, sizeof(stringbuf), "%s/chan-%d-%s-%ld.call", 
			r2chan->r2context->logdir ? r2chan->r2context->logdir : "", 
			r2chan->number, 
			backward ? "backward" : "forward",
			r2chan->call_count++);
	if (res >= sizeof(stringbuf)) {
		openr2_log(r2chan, OR2_LOG_WARNING, "Failed to create file name of length %d.\n", res);
		return;
	} 
	/* sanity check */
	if (r2chan->logfile) {
		openr2_log(r2chan, OR2_LOG_WARNING, "Yay, still have a log file, closing ...\n");
		res = fclose(r2chan->logfile);
		if (res) {
			myerrno = errno;
			EMI(r2chan)->on_os_error(r2chan, myerrno);
			openr2_log(r2chan, OR2_LOG_ERROR, "fclose failed: %s\n", strerror(myerrno));
		}
		r2chan->logfile = NULL;
	}
	r2chan->logfile = fopen(stringbuf, "w");
	if (!r2chan->logfile) {
		myerrno = errno;
		EMI(r2chan)->on_os_error(r2chan, myerrno);
		openr2_log(r2chan, OR2_LOG_ERROR, "fopen failed: %s\n", strerror(myerrno));
	} else {
		currtime = time(NULL);
		if (ctime_r(&currtime, timestr)) {
			timestr[strlen(timestr)-1] = 0; /* remove end of line */
			openr2_log(r2chan, OR2_LOG_DEBUG, "Call started at %s on chan %d\n", timestr, r2chan->number);
		}
	}
}

static void handle_incoming_call(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	if (r2chan->call_files) {
		open_logfile(r2chan, 1);
	}
	/* we have received the line seize, we expect the first MF tone. 
	   let's init our MF engine, if we fail initing the MF engine
	   there is no point sending the seize ack, lets ignore the
	   call, the other end should timeout anyway */
	if (!MFI(r2chan)->mf_write_init(r2chan->mf_write_handle, 0)) {
		openr2_log(r2chan, OR2_LOG_ERROR, "Failed to init MF writer\n");
		handle_protocol_error(r2chan, OR2_INTERNAL_ERROR);
		return;
	}
	if (!MFI(r2chan)->mf_read_init(r2chan->mf_read_handle, 1)) {
		openr2_log(r2chan, OR2_LOG_ERROR, "Failed to init MF reader\n");
		handle_protocol_error(r2chan, OR2_INTERNAL_ERROR);
		return;
	}
	if (set_abcd_signal(r2chan, OR2_ABCD_SEIZE_ACK)) {
		openr2_log(r2chan, OR2_LOG_ERROR, "Failed to send seize ack!, incoming call not proceeding!\n");
		handle_protocol_error(r2chan, OR2_INTERNAL_ERROR);
		return;
	}
	r2chan->r2_state = OR2_SEIZE_ACK_TXD;
	r2chan->mf_state = OR2_MF_SEIZE_ACK_TXD;
	r2chan->mf_group = OR2_MF_BACK_INIT;
	r2chan->direction = OR2_DIR_BACKWARD;
	/* notify the user that a new call is starting to arrive */
	EMI(r2chan)->on_call_init(r2chan);
}

static void mf_fwd_safety_timeout_expired(openr2_chan_t *r2chan, void *data)
{
	OR2_CHAN_STACK;
	handle_protocol_error(r2chan, OR2_FWD_SAFETY_TIMEOUT);
}

static void prepare_mf_tone(openr2_chan_t *r2chan, int tone)
{
	OR2_CHAN_STACK;
	int flush_write = ZT_FLUSH_WRITE, ret;
	int myerrno = 0;
	/* put silence only if we have a write tone */
	if (!tone && r2chan->mf_write_tone) {
		openr2_log(r2chan, OR2_LOG_MF_TRACE, "MF Tx >> %c [OFF]\n", r2chan->mf_write_tone);
		if (ioctl(r2chan->fd, ZT_FLUSH, &flush_write)) {
			myerrno = errno;
			EMI(r2chan)->on_os_error(r2chan, myerrno);
			openr2_log(r2chan, OR2_LOG_ERROR, "ZT_FLUSH failed: %s\n", strerror(myerrno));
			return;
		}
	} 
	/* just choose the tone if the last chosen tone is different */
	if (r2chan->mf_write_tone != tone) {
		ret = MFI(r2chan)->mf_select_tone(r2chan->mf_write_handle, tone);
		if (-1 == ret) {
			/* this is not a protocol error, but there is nothing else we can do anyway */
			openr2_log(r2chan, OR2_LOG_ERROR, "failed to select MF tone\n");
			handle_protocol_error(r2chan, OR2_INTERNAL_ERROR);
			return;
		}
		if (tone) {
			openr2_log(r2chan, OR2_LOG_MF_TRACE, "MF Tx >> %c [ON]\n", tone);
		}	
		r2chan->mf_write_tone = tone;
	}	
}

static void mf_send_dnis(openr2_chan_t *r2chan)
{
	/* TODO: Handle sending of previous DNIS digits */
	OR2_CHAN_STACK;
	/* if there are still some DNIS to send out */
	if ( *r2chan->dnis_ptr ) {
		openr2_log(r2chan, OR2_LOG_DEBUG, "Sending DNIS digit %c\n", *r2chan->dnis_ptr);
		r2chan->mf_state = OR2_MF_DNIS_TXD;
		prepare_mf_tone(r2chan, *r2chan->dnis_ptr);
		r2chan->dnis_ptr++;
	/* if no more DNIS, and there is a signal for it, use it */
	} else if ( GI_TONE(r2chan).no_more_dnis_available ) {
		openr2_log(r2chan, OR2_LOG_DEBUG, "Sending unavailable DNIS signal\n");
		r2chan->mf_state = OR2_MF_DNIS_END_TXD;
		prepare_mf_tone(r2chan, GI_TONE(r2chan).no_more_dnis_available);
	} else {
		openr2_log(r2chan, OR2_LOG_DEBUG, "No more DNIS. Doing nothing, waiting for timeout.\n");
		/* the callee should timeout to detect end of DNIS and
		   resume the MF signaling */
		r2chan->mf_state = OR2_MF_WAITING_TIMEOUT;
		/* even when we are waiting the other end to timeout we
		   cannot wait forever, put a timer to make sure of that */
		openr2_chan_set_timer(r2chan, TIMER(r2chan).mf_fwd_safety, mf_fwd_safety_timeout_expired, NULL);
	}
}

static void report_call_disconnection(openr2_chan_t *r2chan, openr2_call_disconnect_cause_t cause)
{
	OR2_CHAN_STACK;
	openr2_log(r2chan, OR2_LOG_NOTICE, "Far end disconnected. Reason: %s\n", openr2_proto_get_disconnect_string(cause));
	r2chan->call_state = OR2_CALL_DISCONNECTED;
	EMI(r2chan)->on_call_disconnect(r2chan, cause);
}

static void report_call_end(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	openr2_log(r2chan, OR2_LOG_DEBUG, "Call ended\n");
	openr2_proto_set_idle(r2chan);
	EMI(r2chan)->on_call_end(r2chan);
}

static void r2_metering_pulse(openr2_chan_t *r2chan, void *data)
{
	OR2_CHAN_STACK;
	report_call_disconnection(r2chan, OR2_CAUSE_NORMAL_CLEARING);
}

int openr2_proto_handle_abcd_change(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	int abcd, res, myerrno;
	res = ioctl(r2chan->fd, ZT_GETRXBITS, &abcd);
	if (res) {
		myerrno = errno;
		EMI(r2chan)->on_os_error(r2chan, myerrno);
		openr2_log(r2chan, OR2_LOG_ERROR, "ZT_GETRXBITS failed: %s\n", strerror(myerrno));
		return -1;
	}
	openr2_log(r2chan, OR2_LOG_CAS_TRACE, "ABCD Rx << 0x%X\n", abcd);
	/* pick up only the R2 bits */
	abcd &= r2chan->r2context->abcd_r2_bits;
	/* If the R2 bits are the same as the last time we read
	   just ignore them */
	if (r2chan->abcd_read == abcd) {
		openr2_log(r2chan, OR2_LOG_DEBUG, "No change in bits\n");
		return 0;
	} else {
		openr2_log(r2chan, OR2_LOG_DEBUG, "Bits changed from 0x%X to 0x%X\n", r2chan->abcd_read, abcd);
	}
	r2chan->abcd_read = abcd;
	/* ok, bits have changed, we need to know in which 
	   ABCD state we are to know what to do */
	switch (r2chan->r2_state) {
	case OR2_IDLE:
		if (abcd == r2chan->r2context->abcd_signals[OR2_ABCD_BLOCK]) {
			EMI(r2chan)->on_line_blocked(r2chan);
			return 0;
		}
		if (abcd == r2chan->r2context->abcd_signals[OR2_ABCD_IDLE]) {
			EMI(r2chan)->on_line_idle(r2chan);
			return 0;
		}
		if (abcd == r2chan->r2context->abcd_signals[OR2_ABCD_SEIZE]) {
			/* we are in IDLE and just received a seize request
			   lets handle this new call */
			handle_incoming_call(r2chan);
		} else {
			handle_protocol_error(r2chan, OR2_INVALID_CAS_BITS);
		}
		break;
	case OR2_SEIZE_ACK_TXD:
	case OR2_ANSWER_TXD:
		/* if call setup already started or the call is answered 
		   the only valid bit pattern is a clear forward, everything
		   else is protocol error */
		if (abcd == r2chan->r2context->abcd_signals[OR2_ABCD_CLEAR_FORWARD]) {
			r2chan->r2_state = OR2_CLEAR_FWD_RXD;
			report_call_disconnection(r2chan, OR2_CAUSE_NORMAL_CLEARING);
		} else {
			handle_protocol_error(r2chan, OR2_INVALID_CAS_BITS);
		}
		break;
	case OR2_SEIZE_TXD:
		/* if we transmitted a seize we expect the seize ACK */
		if (abcd == r2chan->r2context->abcd_signals[OR2_ABCD_SEIZE_ACK]) {
			/* When the other side send us the seize ack, MF tones
			   can start, we start transmitting DNIS */
			openr2_chan_cancel_timer(r2chan);
			r2chan->r2_state = OR2_SEIZE_ACK_RXD;
			r2chan->mf_group = OR2_MF_GI;
			MFI(r2chan)->mf_write_init(r2chan->mf_write_handle, 1);
			MFI(r2chan)->mf_read_init(r2chan->mf_read_handle, 0);
			mf_send_dnis(r2chan);
		} else {
			handle_protocol_error(r2chan, OR2_INVALID_CAS_BITS);
		}
		break;
	case OR2_CLEAR_BACK_TXD:
		if (abcd == r2chan->r2context->abcd_signals[OR2_ABCD_CLEAR_FORWARD]) {
			report_call_end(r2chan);
		} else {
			handle_protocol_error(r2chan, OR2_INVALID_CAS_BITS);
		}
		break;
	case OR2_ACCEPT_RXD:
		/* once we got MF ACCEPT tone, we expect the ABCD Answer */
		if (abcd == r2chan->r2context->abcd_signals[OR2_ABCD_ANSWER]) {
			openr2_chan_cancel_timer(r2chan);
			r2chan->r2_state = OR2_ANSWER_RXD;
			r2chan->call_state = OR2_CALL_ANSWERED;
			r2chan->mf_state = OR2_MF_OFF_STATE;
			r2chan->answered = 1;
			EMI(r2chan)->on_call_answered(r2chan);
		} else if (abcd == r2chan->r2context->abcd_signals[OR2_ABCD_CLEAR_BACK]) {
			report_call_disconnection(r2chan, OR2_CAUSE_NORMAL_CLEARING);
		} else {
			handle_protocol_error(r2chan, OR2_INVALID_CAS_BITS);
		}
		break;
	case OR2_SEIZE_ACK_RXD:
		if (abcd == r2chan->r2context->abcd_signals[OR2_ABCD_ANSWER]) {
			/* sometimes, since ABCD signaling is faster than MF detectors we
			   may receive the ANSWER signal before actually receiving the
			   MF tone that indicates the call has been accepted (OR2_ACCEPT_RXD). We
			   must not turn off the tone detector because the tone off
			   condition is still missing */
			r2chan->r2_state = OR2_ANSWER_RXD_MF_PENDING;
		} else if (abcd == r2chan->r2context->abcd_signals[OR2_ABCD_CLEAR_BACK]) {
			/* since Seize ACK and Clear Back have the same bit pattern I don't think we
			   ever can fall into this state, can we? */
			openr2_log(r2chan, OR2_LOG_ERROR, "Wah! clear back before answering, why did this happen?.\n");
		} else {
			handle_protocol_error(r2chan, OR2_INVALID_CAS_BITS);
		}
		break;
	case OR2_ANSWER_RXD_MF_PENDING:
	case OR2_ANSWER_RXD:
		if (abcd == r2chan->r2context->abcd_signals[OR2_ABCD_CLEAR_BACK]) {
			r2chan->r2_state = OR2_CLEAR_BACK_RXD;
			if (TIMER(r2chan).r2_metering_pulse) {
				/* if the variant may have metering pulses, this clear back could be not really
				   a clear back but a metering pulse, lets put the timer. If the ABCD signal does not
				   come back to ANSWER then is really a clear back */
				openr2_chan_set_timer(r2chan, TIMER(r2chan).r2_metering_pulse, r2_metering_pulse, NULL);
			} else {
				report_call_disconnection(r2chan, OR2_CAUSE_NORMAL_CLEARING);
			}
		} else {
			handle_protocol_error(r2chan, OR2_INVALID_CAS_BITS);
		}
		break;
	case OR2_CLEAR_BACK_TONE_RXD:
		if (abcd == r2chan->r2context->abcd_signals[OR2_ABCD_IDLE]) {
			openr2_proto_set_idle(r2chan);
		} else {
			handle_protocol_error(r2chan, OR2_INVALID_CAS_BITS);
		}
		break;
	case OR2_CLEAR_FWD_TXD:
		if (abcd == r2chan->r2context->abcd_signals[OR2_ABCD_IDLE]) {
			report_call_end(r2chan);
		} else {
			handle_protocol_error(r2chan, OR2_INVALID_CAS_BITS);
		}
		break;
	case OR2_CLEAR_BACK_RXD:
		/* we got clear back but we have not transmitted clear fwd yet, then, the only
		   reason for ABCD change is a possible metering pulse, if we are not detecting a metering
		   pulse then is a protocol error */
		if (TIMER(r2chan).r2_metering_pulse && abcd == r2chan->r2context->abcd_signals[OR2_ABCD_ANSWER]) {
			/* cancel the metering timer and let's pretend this never happened */
			openr2_chan_cancel_timer(r2chan);
			r2chan->r2_state = OR2_ANSWER_RXD;
			openr2_log(r2chan, OR2_LOG_NOTICE, "Metering pulse received");
		} else {
			handle_protocol_error(r2chan, OR2_INVALID_CAS_BITS);
		}
		break;
	case OR2_BLOCKED:
		openr2_log(r2chan, OR2_LOG_NOTICE, "Doing nothing on ABCD change, we're blocked.\n");
		break;
	default:
		handle_protocol_error(r2chan, OR2_INVALID_R2_STATE);
	}
	return 0;
}

static const char *get_string_from_mode(openr2_call_mode_t mode)
{
	switch (mode) {
	case OR2_CALL_WITH_CHARGE:
		return "Call With Charge";
	case OR2_CALL_NO_CHARGE:
		return "Call With No Charge";
	case OR2_CALL_SPECIAL:
		return "Special Call";
	default:
		return "*UNKNOWN*";
	}
}

static int get_tone_from_mode(openr2_chan_t *r2chan, openr2_call_mode_t mode)
{
	switch (mode) {
	case OR2_CALL_WITH_CHARGE:
		return GB_TONE(r2chan).accept_call_with_charge;
	case OR2_CALL_NO_CHARGE:
		return GB_TONE(r2chan).accept_call_no_charge;
	case OR2_CALL_SPECIAL:
		return GB_TONE(r2chan).special_info_tone;
	default:
		openr2_log(r2chan, OR2_LOG_WARNING, "Unkown call mode (%d), defaulting to %s\n", get_string_from_mode(OR2_CALL_NO_CHARGE));
		return GB_TONE(r2chan).accept_call_no_charge;
	}
}

int openr2_proto_accept_call(openr2_chan_t *r2chan, openr2_call_mode_t mode)
{
	OR2_CHAN_STACK;
	if (OR2_CALL_OFFERED != r2chan->call_state) {
		openr2_log(r2chan, OR2_LOG_WARNING, "Cannot accept call if the call has not been offered!\n");
		return -1;
	}
	r2chan->mf_state = OR2_MF_ACCEPTED_TXD;
	prepare_mf_tone(r2chan, get_tone_from_mode(r2chan, mode));		
	return 0;
}

int openr2_proto_answer_call(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	if (r2chan->call_state != OR2_CALL_ACCEPTED) {
		openr2_log(r2chan, OR2_LOG_WARNING, "Cannot answer call if the call is not accepted first\n");
		return -1;
	}
	if (set_abcd_signal(r2chan, OR2_ABCD_ANSWER)) {
		openr2_log(r2chan, OR2_LOG_ERROR, "Cannot send ANSWER signal, failed to answer call!\n");
		return -1;
	}
	r2chan->call_state = OR2_CALL_ANSWERED;
	r2chan->r2_state = OR2_ANSWER_TXD;
	EMI(r2chan)->on_call_answered(r2chan);
	r2chan->answered = 1;
	return 0;
}

static void request_calling_party_category(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	int tone = GA_TONE(r2chan).request_category ? GA_TONE(r2chan).request_category : GA_TONE(r2chan).request_category_and_change_to_gc;
	r2chan->mf_group = GA_TONE(r2chan).request_category ? OR2_MF_GA : OR2_MF_GC;
	r2chan->mf_state = OR2_MF_CATEGORY_RQ_TXD;
	prepare_mf_tone(r2chan, tone);
}

static void set_silence(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	prepare_mf_tone(r2chan, 0);
	r2chan->mf_write_tone = 0;
}

static void mf_back_resume_cycle(openr2_chan_t *r2chan, void *data)
{
	OR2_CHAN_STACK;
	set_silence(r2chan);
}

static void request_change_to_g2(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	/* request to change to group 2 can come from either from Group C (only for Mexico)
	   or Group A (All the world, including Mexico) */
	int change_tone = (OR2_MF_GC == r2chan->mf_group) ? GC_TONE(r2chan).request_change_to_g2
		                                          : GA_TONE(r2chan).request_change_to_g2;
	r2chan->mf_group = OR2_MF_GB;
	r2chan->mf_state = OR2_MF_CHG_GII_TXD;
	openr2_log(r2chan, OR2_LOG_DEBUG, "Requesting change to Group II with signal 0x%X\n", change_tone);
	prepare_mf_tone(r2chan, change_tone);
}

static void mf_back_cycle_timeout_expired(openr2_chan_t *r2chan, void *data)
{
	OR2_CHAN_STACK;
	if (OR2_MF_TONE_INVALID == GI_TONE(r2chan).no_more_dnis_available
	     && r2chan->mf_group == OR2_MF_GA
	     && r2chan->mf_state == OR2_MF_DNIS_RQ_TXD) {
		openr2_log(r2chan, OR2_LOG_DEBUG, "MF cycle timed out, no more DNIS\n");
		/* the other end has run out of DNIS digits and were in a R2 variant that
		   does not support 'No More DNIS available' signal (ain't that silly?), and
		   those R2 variants let the backward end to timeout and resume the MF dance,
		   that's why we timed out waiting for more DNIS. Let's resume the MF signaling
		   and ask the calling party category (if needed). Since they are now in a silent 
		   state we will not get a 'tone off' condition, hence we need a timeout to mute 
		   our tone */
		openr2_chan_set_timer(r2chan, TIMER(r2chan).mf_back_resume_cycle, mf_back_resume_cycle, NULL);
		if (!r2chan->r2context->get_ani_first) {
			/* we were not asked to get the ANI first, hence when this
		           timeout occurs we know for sure we have not retrieved ANI yet,
		           let's retrieve it now. */
			request_calling_party_category(r2chan);
		} else {
			/* ANI must have been retrieved already (before DNIS),
			   let's go directly to GII, the final stage */
			request_change_to_g2(r2chan);
		}
	} else {
		openr2_log(r2chan, OR2_LOG_WARNING, "MF back cycle timed out!\n");
		handle_protocol_error(r2chan, OR2_BACK_MF_TIMEOUT);
	}	
}

static void request_next_dnis_digit(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	openr2_mf_tone_t request_tone = (OR2_MF_GC == r2chan->mf_group) 
		                      ? GC_TONE(r2chan).request_next_dnis_digit_and_change_to_ga
		                      : GA_TONE(r2chan).request_next_dnis_digit;
	r2chan->mf_group = OR2_MF_GA;
	r2chan->mf_state = OR2_MF_DNIS_RQ_TXD;
	openr2_log(r2chan, OR2_LOG_DEBUG, "Requesting next DNIS with signal 0x%X.\n", request_tone);
	prepare_mf_tone(r2chan, request_tone);
}

static void mf_receive_expected_dnis(openr2_chan_t *r2chan, int tone)
{
	OR2_CHAN_STACK;
	if (OR2_MF_TONE_10 <= tone && OR2_MF_TONE_9 >= tone) {
		openr2_log(r2chan, OR2_LOG_DEBUG, "Getting DNIS digit %c\n", tone);
		r2chan->dnis[r2chan->dnis_len++] = tone;
		r2chan->dnis[r2chan->dnis_len] = '\0';
		openr2_log(r2chan, OR2_LOG_DEBUG, "DNIS so far: %s, expected length: %d\n", r2chan->dnis, r2chan->r2context->max_dnis);
		if (DNIS_COMPLETE(r2chan)) {
			openr2_log(r2chan, OR2_LOG_DEBUG, "Done getting DNIS!\n");
			/* if this is the first and last DNIS digit we have or
			   we were not required to get the ANI first, request it now, 
			   otherwise is time to go to GII signals */
			if (1 == r2chan->dnis_len || !r2chan->r2context->get_ani_first) {
				request_calling_party_category(r2chan);
			} else {
				request_change_to_g2(r2chan);
			} 
		} else if (1 == r2chan->dnis_len && r2chan->r2context->get_ani_first) {
			request_calling_party_category(r2chan);
		} else {
			request_next_dnis_digit(r2chan);
		}
	} else if (GI_TONE(r2chan).no_more_dnis_available == tone) {
		/* not sure if we ever could get no more dnis as first DNIS tone
		   but let's handle it just in case */
		if (0 == r2chan->dnis_len || !r2chan->r2context->get_ani_first) {
			request_calling_party_category(r2chan);
		} else {
			request_change_to_g2(r2chan);
		}
	} else {
		/* we were supposed to handle DNIS, but the tone
		   is not in the range of valid digits */
		handle_protocol_error(r2chan, OR2_INVALID_MF_TONE);
	}
}

static openr2_calling_party_category_t tone2category(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	if (GII_TONE(r2chan).national_subscriber == r2chan->caller_category) {
		return OR2_CALLING_PARTY_CATEGORY_NATIONAL_SUBSCRIBER;

	} else if (GII_TONE(r2chan).national_priority_subscriber == r2chan->caller_category) {
		return OR2_CALLING_PARTY_CATEGORY_NATIONAL_PRIORITY_SUBSCRIBER;

	} else if (GII_TONE(r2chan).international_subscriber == r2chan->caller_category) {
		return OR2_CALLING_PARTY_CATEGORY_INTERNATIONAL_SUBSCRIBER;

	} else if (GII_TONE(r2chan).international_priority_subscriber == r2chan->caller_category) {
		return OR2_CALLING_PARTY_CATEGORY_INTERNATIONAL_PRIORITY_SUBSCRIBER;

	} else {
		return OR2_CALLING_PARTY_CATEGORY_UNKNOWN;
	}
}

static int category2tone(openr2_chan_t *r2chan, openr2_calling_party_category_t category)
{
	OR2_CHAN_STACK;
	switch (category) {
	case OR2_CALLING_PARTY_CATEGORY_NATIONAL_SUBSCRIBER:
		return GII_TONE(r2chan).national_subscriber;
	case OR2_CALLING_PARTY_CATEGORY_NATIONAL_PRIORITY_SUBSCRIBER:
		return GII_TONE(r2chan).national_priority_subscriber;
	case OR2_CALLING_PARTY_CATEGORY_INTERNATIONAL_SUBSCRIBER:
		return GII_TONE(r2chan).international_subscriber;
	case OR2_CALLING_PARTY_CATEGORY_INTERNATIONAL_PRIORITY_SUBSCRIBER:
		return GII_TONE(r2chan).international_priority_subscriber;
	default:
		return GII_TONE(r2chan).national_subscriber;;
	}
}

static void mf_receive_expected_ani(openr2_chan_t *r2chan, int tone)
{
	OR2_CHAN_STACK;
	int next_ani_request_tone = GC_TONE(r2chan).request_next_ani_digit ? 
		                    GC_TONE(r2chan).request_next_ani_digit : 
				    GA_TONE(r2chan).request_next_ani_digit;
	/* no tone, just request next ANI if needed, otherwise
	   switch to Group B/II  */
	if (!tone || (OR2_MF_TONE_10 <= tone && OR2_MF_TONE_9 >= tone)) {
		/* if we have a tone, save it */
		if (tone) {
			openr2_log(r2chan, OR2_LOG_DEBUG, "Getting ANI digit %c\n", tone);
			r2chan->ani[r2chan->ani_len++] = tone;
			r2chan->ani[r2chan->ani_len] = '\0';
			openr2_log(r2chan, OR2_LOG_DEBUG, "ANI so far: %s, expected length: %d\n", r2chan->ani, r2chan->r2context->max_ani);
		}
		/* if we don't have a tone, or the ANI len is not enough yet, 
		   ask for more ANI */
		if (!tone || r2chan->r2context->max_ani > r2chan->ani_len) {
			r2chan->mf_state = OR2_MF_ANI_RQ_TXD;
			prepare_mf_tone(r2chan, next_ani_request_tone);
		} else {
			openr2_log(r2chan, OR2_LOG_DEBUG, "Done getting ANI!\n");
			if (!r2chan->r2context->get_ani_first || DNIS_COMPLETE(r2chan)) {
				request_change_to_g2(r2chan);
			} else {
				request_next_dnis_digit(r2chan);
			}	
		}
	/* they notify us about no more ANI available or the ANI 
	   is restricted AKA private */
	} else if ( tone == GI_TONE(r2chan).no_more_ani_available ||
		    tone == GI_TONE(r2chan).caller_ani_is_restricted ) {
		openr2_log(r2chan, OR2_LOG_DEBUG, "Got end of ANI\n");
		if ( tone == GI_TONE(r2chan).caller_ani_is_restricted ) {
			openr2_log(r2chan, OR2_LOG_DEBUG, "ANI is restricted\n");
			r2chan->caller_ani_is_restricted = 1;
		}	
		if (!r2chan->r2context->get_ani_first || DNIS_COMPLETE(r2chan)) {
			request_change_to_g2(r2chan);
		} else {
			request_next_dnis_digit(r2chan);
		}	
	} else {
		handle_protocol_error(r2chan, OR2_INVALID_MF_TONE);
	}
}

static void handle_forward_mf_tone(openr2_chan_t *r2chan, int tone)
{
	OR2_CHAN_STACK;
	/* schedule a new timer that will handle the timeout for our tone signal */
	openr2_chan_set_timer(r2chan, TIMER(r2chan).mf_back_cycle, mf_back_cycle_timeout_expired, NULL);
	switch (r2chan->mf_group) {
	/* we just sent the seize ACK and we are starting with the MF dance */
	case OR2_MF_BACK_INIT:
		switch (r2chan->mf_state) {
		case OR2_MF_SEIZE_ACK_TXD:
			/* after sending the seize ack we expect DNIS  */
			mf_receive_expected_dnis(r2chan, tone);
			break;
		default:
			handle_protocol_error(r2chan, OR2_INVALID_MF_STATE);
			break;
		}
		break;
	/* We are now at Group A signals, requesting DNIS or ANI, 
	   depending on the protocol variant */
	case OR2_MF_GA:
		/* ok we're at GROUP A signals. Let's see what was 
		   the last thing we did */
		switch (r2chan->mf_state) {
		/* we requested more DNIS */
		case OR2_MF_DNIS_RQ_TXD:
			/* then we receive more DNIS :) */
			mf_receive_expected_dnis(r2chan, tone);
			break;
		/* we requested the calling party category */
		case OR2_MF_CATEGORY_RQ_TXD:
			r2chan->caller_category = tone;
			if (r2chan->r2context->max_ani > 0) {
				mf_receive_expected_ani(r2chan, 0);
			} else {
				/* switch to Group B/II, we're ready to answer! */
				request_change_to_g2(r2chan);
			}
			break;
		/* we requested more ANI */
		case OR2_MF_ANI_RQ_TXD:
			mf_receive_expected_ani(r2chan, tone);
			break;
		/* hu? WTF we were doing?? */
		default:
			handle_protocol_error(r2chan, OR2_INVALID_MF_STATE);
			break;
		}
		break;
	case OR2_MF_GB:
		switch (r2chan->mf_state) {
		case OR2_MF_CHG_GII_TXD:
			/* we cannot do anything by ourselves. The user has
			   to decide what to do. Let's inform him/her that
			   a new call is ready to be accepted or rejected */
			r2chan->call_state = OR2_CALL_OFFERED;
			EMI(r2chan)->on_call_offered(r2chan, r2chan->ani, r2chan->dnis, tone2category(r2chan));
			break;
		default:
			handle_protocol_error(r2chan, OR2_INVALID_MF_STATE);
			break;
		}
		break;
	/* Viva Mexico Cabrones!, solo Mexico tiene Grupo C ;)  */
	/* Group C is only for Mexico */
	case OR2_MF_GC:
		/* at this point, we either sent a category request, 
		   usually preceding the ANI request or we already sent
		   an ANI request. Anything else, is protocol error */
		switch (r2chan->mf_state) {
		/* we requested the calling party category */
		case OR2_MF_CATEGORY_RQ_TXD:
			r2chan->caller_category = tone;
			if (r2chan->r2context->max_ani > 0) {
				mf_receive_expected_ani(r2chan, 0);
			} else {
				/* switch to Group B/II, we're ready to answer! */
				request_change_to_g2(r2chan);
			}
			break;
		/* we requested more ANI */
		case OR2_MF_ANI_RQ_TXD:
			mf_receive_expected_ani(r2chan, tone);
			break;
		/* yikes, we have an error! */
		default:
			handle_protocol_error(r2chan, OR2_INVALID_MF_STATE);
			break;
		}
		break;
	default:
		handle_protocol_error(r2chan, OR2_INVALID_MF_GROUP);
		break;
	}
}

static void ready_to_answer(openr2_chan_t *r2chan, void *data)
{
	OR2_CHAN_STACK;
	openr2_chan_cancel_timer(r2chan);
	/* mode not important here, the BACKWARD side accepted the call so
	   they already know that. 
	   (we could save the tone they used to accept and pass the call type) */
	EMI(r2chan)->on_call_accepted(r2chan, OR2_CALL_UNKNOWN);
}

static void handle_forward_mf_silence(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	/* we just got silence. Let's silence our side as well.
	   depending on the MF hooks implementation this may be
	   immediate or not */
	set_silence(r2chan);
	switch (r2chan->mf_group) {
	case OR2_MF_GA:
		/* no further action required. The other end should 
		   handle our previous request */
		break;
	case OR2_MF_GB:
		switch (r2chan->mf_state) {
		case OR2_MF_CHG_GII_TXD:
			/* no further action required */
			break;
		case OR2_MF_ACCEPTED_TXD:
			/* MF dance has ended. The call has not been answered
			   but the user must decide when to answer. We don't
			   notify immediately because we still don't stop our
			   tone. I tried waiting for ZT_IOMUX_WRITEEMPTY event
			   but even then, it seems sometimes the other end requires
			   a bit of time to detect our tone off condition. If we
			   notify the user of the call accepted and he tries to
			   answer, setting the ABCD bits before the other end
			   detects our tone off condition can lead to the other
			   end to never really detect our ABCD answer state or
			   consider it a protocol error */
			r2chan->mf_state = OR2_MF_OFF_STATE;
			r2chan->call_state = OR2_CALL_ACCEPTED;
			openr2_chan_set_timer(r2chan, OR2_PROTO_ANSWER_WAIT_TIME, ready_to_answer, NULL);
			break;
		case OR2_MF_DISCONNECT_TXD:	
			/* we did not accept the call and sent some disconnect tone 
			   (busy, network congestion etc). The other end will take care
			   of clearing up the call.  */
			openr2_chan_cancel_timer(r2chan);
			break;
		default:
			handle_protocol_error(r2chan, OR2_INVALID_MF_STATE);
			break;
		}
		break;
	case OR2_MF_GC:
		/* no further action required. The other end should 
		   handle our previous request */
		break;
	default:
		handle_protocol_error(r2chan, OR2_INVALID_MF_GROUP);
	}
}

static void handle_backward_mf_tone(openr2_chan_t *r2chan, int tone)
{
	OR2_CHAN_STACK;
	/* if we had a safety timer, clean it up */
	openr2_chan_cancel_timer(r2chan);

	/* we are the forward side, each time we receive a tone from the
	   backward side, we just mute our tone */
	set_silence(r2chan);
}

static void mf_send_category(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	r2chan->mf_state = OR2_MF_CATEGORY_TXD;
	r2chan->category_sent = 1;
	openr2_log(r2chan, OR2_LOG_DEBUG, "Sending category %s\n", 
			openr2_proto_get_category_string(tone2category(r2chan)));
	prepare_mf_tone(r2chan, r2chan->caller_category);
}

static void mf_send_ani(openr2_chan_t *r2chan)
{
	/* TODO: Handle sending of previous ANI digits */
	OR2_CHAN_STACK;
	/* if the pointer to ANI is null, that means the caller ANI is restricted */
	if (NULL == r2chan->ani_ptr) {
		openr2_log(r2chan, OR2_LOG_DEBUG, "Sending Restricted ANI\n");
		r2chan->mf_state = OR2_MF_DNIS_END_TXD;
		prepare_mf_tone(r2chan, GI_TONE(r2chan).caller_ani_is_restricted);
	/* ok, ANI is not restricted, let's see 
	   if there are still some ANI to send out */
	} else if (*r2chan->ani_ptr) {
		openr2_log(r2chan, OR2_LOG_DEBUG, "Sending ANI digit %c\n", *r2chan->ani_ptr);
		r2chan->mf_state = OR2_MF_ANI_TXD;
		prepare_mf_tone(r2chan, *r2chan->ani_ptr);
		r2chan->ani_ptr++;
	/* if no more ANI, and there is a signal for it, use it */
	} else if (GI_TONE(r2chan).no_more_ani_available) {
		openr2_log(r2chan, OR2_LOG_DEBUG, "Sending more ANI unavailable\n");
		r2chan->mf_state = OR2_MF_DNIS_END_TXD;
		prepare_mf_tone(r2chan, GI_TONE(r2chan).no_more_ani_available);
	} else {
		openr2_log(r2chan, OR2_LOG_DEBUG, "No more ANI, expecting timeout from the other side\n");
		/* the callee should timeout to detect end of ANI and
		   resume the MF signaling */
		r2chan->mf_state = OR2_MF_WAITING_TIMEOUT;
		/* even when we are waiting the other end to timeout we
		   cannot wait forever, put a timer to make sure of that */
		openr2_chan_set_timer(r2chan, TIMER(r2chan).mf_fwd_safety, mf_fwd_safety_timeout_expired, NULL);
	}
}

static void r2_answer_timeout_expired(openr2_chan_t *r2chan, void *data)
{
	OR2_CHAN_STACK;
	report_call_disconnection(r2chan, OR2_CAUSE_NO_ANSWER);
}

static openr2_call_mode_t get_mode_from_tone(openr2_chan_t *r2chan, int tone)
{
	OR2_CHAN_STACK;
	if (tone == GB_TONE(r2chan).accept_call_with_charge) {
		return OR2_CALL_WITH_CHARGE;
	} else if (GB_TONE(r2chan).accept_call_no_charge) {
		return OR2_CALL_NO_CHARGE;
	} else if (GB_TONE(r2chan).special_info_tone) {
		return OR2_CALL_SPECIAL;
	} else {
		openr2_log(r2chan, OR2_LOG_WARNING, "Unknown call type\n");
		return OR2_CALL_UNKNOWN;
	}	
}

static void handle_accept_tone(openr2_chan_t *r2chan, openr2_call_mode_t mode)
{
	OR2_CHAN_STACK;
	openr2_mf_state_t previous_mf_state;
	openr2_call_state_t previous_call_state;
        if (r2chan->r2_state == OR2_ANSWER_RXD_MF_PENDING) {
                /* they answered before we even detected they accepted,
                   lets just call on_call_accepted and immediately
                   on_call_answered */

                /* first accepted */
		previous_mf_state = r2chan->mf_state;
		previous_call_state = r2chan->mf_state;
                r2chan->r2_state = OR2_ACCEPT_RXD;
                EMI(r2chan)->on_call_accepted(r2chan, mode);

		/* if the on_call_accepted callback calls some openr2 API
		   it can change the state and we no longer want to continue answering */
		if (r2chan->r2_state != OR2_ACCEPT_RXD 
		    || r2chan->mf_state != previous_mf_state
		    || r2chan->call_state != previous_call_state) {
			openr2_log(r2chan, OR2_LOG_NOTICE, "Not proceeding with ANSWERED callback\n");
			return;
		}
                /* now answered */
                openr2_chan_cancel_timer(r2chan);
                r2chan->r2_state = OR2_ANSWER_RXD;
                r2chan->call_state = OR2_CALL_ANSWERED;
                r2chan->mf_state = OR2_MF_OFF_STATE;
                r2chan->answered = 1;
                EMI(r2chan)->on_call_answered(r2chan);
        } else {
                /* They have accepted the call. We do nothing but
                   wait for answer. */
                r2chan->r2_state = OR2_ACCEPT_RXD;
                openr2_chan_set_timer(r2chan, TIMER(r2chan).r2_answer, r2_answer_timeout_expired, NULL);
                EMI(r2chan)->on_call_accepted(r2chan, mode);
        }
}

static void handle_group_a_request(openr2_chan_t *r2chan, int tone)
{
	OR2_CHAN_STACK;
	openr2_mf_tone_t request_category_tone = GA_TONE(r2chan).request_category ?
						 GA_TONE(r2chan).request_category :
						 GA_TONE(r2chan).request_category_and_change_to_gc;
	if (tone == GA_TONE(r2chan).request_next_dnis_digit) {
		mf_send_dnis(r2chan);
	} else if (r2chan->category_sent && (tone == GA_TONE(r2chan).request_next_ani_digit)) {
		mf_send_ani(r2chan);
	} else if (tone == request_category_tone) {
		if (request_category_tone == GA_TONE(r2chan).request_category_and_change_to_gc) {
			r2chan->mf_group = OR2_MF_GIII;
		}
		mf_send_category(r2chan);
	} else if (tone == GA_TONE(r2chan).request_change_to_g2) {
		r2chan->mf_group = OR2_MF_GII;
		mf_send_category(r2chan);
        } else if (tone == GA_TONE(r2chan).address_complete_charge_setup) {
		handle_accept_tone(r2chan, OR2_CALL_WITH_CHARGE);
	} else if (tone == GA_TONE(r2chan).network_congestion) {
		r2chan->r2_state = OR2_CLEAR_BACK_TONE_RXD;
		report_call_disconnection(r2chan, OR2_CAUSE_NETWORK_CONGESTION);
	} else {
		handle_protocol_error(r2chan, OR2_INVALID_MF_TONE);
	}
}

static void handle_group_c_request(openr2_chan_t *r2chan, int tone)
{
	OR2_CHAN_STACK;
	if (tone == GC_TONE(r2chan).request_next_ani_digit) {
		mf_send_ani(r2chan);
	} else if (tone == GC_TONE(r2chan).request_change_to_g2) {
		/* requesting change to Group II means we should
		   send the calling party category again?  */
		r2chan->mf_group = OR2_MF_GII;
		mf_send_category(r2chan);
	} else if (tone == GC_TONE(r2chan).request_next_dnis_digit_and_change_to_ga) {
		r2chan->mf_group = OR2_MF_GI;
		mf_send_dnis(r2chan);
	} else {
		handle_protocol_error(r2chan, OR2_INVALID_MF_TONE);
	}
}

static void handle_group_b_request(openr2_chan_t *r2chan, int tone)
{
	OR2_CHAN_STACK;
	if (tone == GB_TONE(r2chan).accept_call_with_charge 
	    || tone == GB_TONE(r2chan).accept_call_no_charge
	    || tone == GB_TONE(r2chan).special_info_tone) {
	    handle_accept_tone(r2chan, get_mode_from_tone(r2chan, tone));
	} else if (tone == GB_TONE(r2chan).busy_number){
		r2chan->r2_state = OR2_CLEAR_BACK_TONE_RXD;
		report_call_disconnection(r2chan, OR2_CAUSE_BUSY_NUMBER);
	} else if (tone == GB_TONE(r2chan).network_congestion) {
		r2chan->r2_state = OR2_CLEAR_BACK_TONE_RXD;
		report_call_disconnection(r2chan, OR2_CAUSE_NETWORK_CONGESTION);
	} else if (tone == GB_TONE(r2chan).unallocated_number) {
		r2chan->r2_state = OR2_CLEAR_BACK_TONE_RXD;
		report_call_disconnection(r2chan, OR2_CAUSE_UNALLOCATED_NUMBER);
	} else if (tone == GB_TONE(r2chan).line_out_of_order) {
		r2chan->r2_state = OR2_CLEAR_BACK_TONE_RXD;
		report_call_disconnection(r2chan, OR2_CAUSE_OUT_OF_ORDER);
	} else {
		handle_protocol_error(r2chan, OR2_INVALID_MF_TONE);
	}
}

static void handle_backward_mf_silence(openr2_chan_t *r2chan, int tone)
{
	OR2_CHAN_STACK;
	/* the backward side has muted its tone, it is time to take
	   action depending on the tone they sent */
	switch (r2chan->mf_group) {
	case OR2_MF_GI:
		handle_group_a_request(r2chan, tone);
		break;
	case OR2_MF_GII:
		handle_group_b_request(r2chan, tone);
		break;
	case OR2_MF_GIII:
		handle_group_c_request(r2chan, tone);
		break;
	default:
		handle_protocol_error(r2chan, OR2_INVALID_MF_GROUP);
		break;
	}
}

static int timediff(struct timeval *t1, struct timeval *t2)
{
	int secdiff = 0;
	int msdiff = 0;
	if (t1->tv_sec == t2->tv_sec) {
		return ((t1->tv_usec - t2->tv_usec)/1000);
	}
	secdiff = t1->tv_sec - t2->tv_sec;	
	secdiff--;
	msdiff  = (t1->tv_usec) ? (t1->tv_usec/1000)          : 0;
	msdiff += (t2->tv_usec) ? (1000 - (t2->tv_usec/1000)) : 0;
	return msdiff;
}

static int check_threshold(openr2_chan_t *r2chan, int tone)
{
	OR2_CHAN_STACK;
	int res = 0;
	int tone_threshold = 0;
	struct timeval currtime = {0, 0};
	if (r2chan->r2context->mf_threshold) {
		if (r2chan->mf_threshold_tone != tone) {
			res = gettimeofday(&r2chan->mf_threshold_time, NULL);
			if (-1 == res) {
				openr2_log(r2chan, OR2_LOG_ERROR, "gettimeofday failed when setting threshold time\n");
				return -1;
			}
			r2chan->mf_threshold_tone = tone;
		}
		res = gettimeofday(&currtime, NULL);
		if (-1 == res) {
			openr2_log(r2chan, OR2_LOG_ERROR, "gettimeofday failed when checking tone length\n");
			return -1;
		}
		tone_threshold = timediff(&currtime, &r2chan->mf_threshold_time);
		if (tone_threshold < r2chan->r2context->mf_threshold) {
			if (tone) {
				openr2_log(r2chan, OR2_LOG_DEBUG, "Tone %c ignored\n", tone);
			} else {
				openr2_log(r2chan, OR2_LOG_DEBUG, "Tone off ignored\n");
			}	
			return -1;
		}
	}
	return 0;
}

void openr2_proto_handle_mf_tone(openr2_chan_t *r2chan, int tone)
{
	OR2_CHAN_STACK;
	if (tone) {

		/* since we get a continuous tone, this tone might be one that we
		   already handled but since the other end has not detected our tone
		   has not stopped its own tone. If it is the same
		   just ignore it, we already handled it */
		if (r2chan->mf_read_tone == tone) {
			return;
		}

		/* safety check. Each rx tone should be muted before changing the tone
		   hence the read tone right now should be 0 */
		if (r2chan->mf_read_tone != 0) {
			openr2_log(r2chan, OR2_LOG_ERROR, "Broken MF sequence got %c but never got tone off for tone %c!\n", tone, r2chan->mf_read_tone);
			handle_protocol_error(r2chan, OR2_BROKEN_MF_SEQUENCE);
			return;
		}

		/* do threshold checking if enabled */
		if (check_threshold(r2chan, tone)) {
			return;
		}

		openr2_log(r2chan, OR2_LOG_MF_TRACE, "MF Rx << %c [ON]\n", tone);
		r2chan->mf_read_tone = tone;

		/* handle the incoming MF tone */
		if ( OR2_DIR_BACKWARD == r2chan->direction ) {
			handle_forward_mf_tone(r2chan, tone);
		} else if ( OR2_DIR_FORWARD == r2chan->direction ) {
			handle_backward_mf_tone(r2chan, tone);
		} else {
			openr2_log(r2chan, OR2_LOG_ERROR, "BUG: invalid direction of R2 channel\n");
			handle_protocol_error(r2chan, OR2_LIBRARY_BUG);
		}

	} else {

		/* If we already detected the silence condition, ignore this one */
		if (0 == r2chan->mf_read_tone) {
			return;
		}
		if (check_threshold(r2chan, 0)) {
			return;
		}
		/* handle the silence condition */
		openr2_log(r2chan, OR2_LOG_MF_TRACE, "MF Rx << %c [OFF]\n", r2chan->mf_read_tone);
		if (OR2_DIR_BACKWARD == r2chan->direction) {
			handle_forward_mf_silence(r2chan);
		} else if (OR2_DIR_FORWARD == r2chan->direction) {
			/* when we are in forward we take action when the other side
			   silence its tone, not when receiving the tone */
			handle_backward_mf_silence(r2chan, r2chan->mf_read_tone);
		} else {
			openr2_log(r2chan, OR2_LOG_ERROR, "BUG: invalid direction of R2 channel\n");
			handle_protocol_error(r2chan, OR2_LIBRARY_BUG);
		}
		r2chan->mf_read_tone = 0;

	}
}

static void seize_timeout_expired(openr2_chan_t *r2chan, void *data)
{
	OR2_CHAN_STACK;
	openr2_log(r2chan, OR2_LOG_WARNING, "Seize Timeout Expired!\n");
	handle_protocol_error(r2chan, OR2_SEIZE_TIMEOUT);
}

int openr2_proto_make_call(openr2_chan_t *r2chan, const char *ani, const char *dnis, openr2_calling_party_category_t category)
{
	OR2_CHAN_STACK;
	const char *digit;
	int copy_ani = 1;
	int copy_dnis = 1;
	openr2_log(r2chan, OR2_LOG_DEBUG, "Attempting to make call (ANI=%s, DNIS=%s, category=%s)\n", ani, dnis, openr2_proto_get_category_string(category));
	/* we can dial only if we're in IDLE */
	if (r2chan->call_state != OR2_CALL_IDLE) {
		openr2_log(r2chan, OR2_LOG_ERROR, "Call state should be IDLE but is '%s'\n", openr2_proto_get_call_state_string(r2chan));
		return -1;
	}
	/* try to handle last minute changes if any. 
	   This will detect IDLE lines if the last time the user checked
	   it was in some other state */
	openr2_proto_handle_abcd_change(r2chan);
	if (r2chan->abcd_read != r2chan->r2context->abcd_signals[OR2_ABCD_IDLE]) {
		openr2_log(r2chan, OR2_LOG_ERROR, "Trying to dial out in a non-idle channel\n");
		return -1;
	}
	/* make sure both ANI and DNIS are numeric */
	digit = ani;
	while (*digit) {
		if (!isdigit(*digit)) {
			openr2_log(r2chan, OR2_LOG_NOTICE, "Char '%c' is not a digit, ANI will not be sent.\n", *digit);	
			copy_ani = 0;
			break;
		}
		digit++;
	}
	digit = dnis;
	while (*digit) {
		if (!isdigit(*digit)) {
			openr2_log(r2chan, OR2_LOG_NOTICE, "Char '%c' is not a digit, DNIS will not be sent.\n", *digit);	
			copy_dnis = 0;
			break;
			/* should we proceed with the call without DNIS? */
		}
		digit++;
	}
	if (r2chan->call_files) {
		open_logfile(r2chan, 0);
	}
	if (set_abcd_signal(r2chan, OR2_ABCD_SEIZE)) {
		openr2_log(r2chan, OR2_LOG_ERROR, "Failed to seize line!, cannot make a call!\n");
		return -1;
	}
	r2chan->call_state = OR2_CALL_DIALING;
	r2chan->r2_state = OR2_SEIZE_TXD;
	r2chan->mf_group = OR2_MF_FWD_INIT;
	r2chan->direction = OR2_DIR_FORWARD;
	r2chan->caller_category = category2tone(r2chan, category);
	if (copy_ani) {
		strncpy(r2chan->ani, ani, sizeof(r2chan->ani)-1);
		r2chan->ani[sizeof(r2chan->ani)-1] = '\0';
	} else {
		r2chan->ani[0] = '\0';
	}
	r2chan->ani_ptr = r2chan->ani;
	if (copy_dnis) {
		strncpy(r2chan->dnis, dnis, sizeof(r2chan->dnis)-1);
		r2chan->dnis[sizeof(r2chan->dnis)-1] = '\0';
	} else {
		r2chan->dnis[0] = '\0';
	}	
	r2chan->dnis_ptr = r2chan->dnis;
	/* cannot wait forever for seize ack, put a timer */
	openr2_chan_set_timer(r2chan, TIMER(r2chan).r2_seize, seize_timeout_expired, NULL);
	return 0;
}

static void send_disconnect(openr2_chan_t *r2chan, openr2_call_disconnect_cause_t cause)
{
	int tone;
	r2chan->mf_state = OR2_MF_DISCONNECT_TXD;
	switch (cause) {
	case OR2_CAUSE_BUSY_NUMBER:
		tone = GB_TONE(r2chan).busy_number;
		break;
	case OR2_CAUSE_NETWORK_CONGESTION:
		tone = GB_TONE(r2chan).network_congestion;
		break;
	case OR2_CAUSE_UNALLOCATED_NUMBER:
		tone = GB_TONE(r2chan).unallocated_number;
		break;
	case OR2_CAUSE_OUT_OF_ORDER:
		tone = GB_TONE(r2chan).line_out_of_order;
		break;
	default:
		tone = GB_TONE(r2chan).line_out_of_order;
		break;
	}
	prepare_mf_tone(r2chan, tone);
}

static int send_clear_forward(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	r2chan->r2_state = OR2_CLEAR_FWD_TXD;
	r2chan->mf_state = OR2_MF_OFF_STATE;
	return set_abcd_signal(r2chan, OR2_ABCD_CLEAR_FORWARD);
}

static int send_clear_backward(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	r2chan->r2_state = OR2_CLEAR_BACK_TXD;
	r2chan->mf_state = OR2_MF_OFF_STATE;
	return set_abcd_signal(r2chan, OR2_ABCD_CLEAR_BACK);	
}

int openr2_proto_disconnect_call(openr2_chan_t *r2chan, openr2_call_disconnect_cause_t cause)
{
	OR2_CHAN_STACK;
	/* cannot drop a call when there is none to drop */
	if (r2chan->call_state == OR2_CALL_IDLE) {
		return -1;
	}
	if (r2chan->direction == OR2_DIR_BACKWARD) {
		if (r2chan->call_state == OR2_CALL_OFFERED) {
			/* if the call has been offered we need to give a reason 
			   to disconnect using a MF tone. That should make the other
			   end send us a clear forward  */
			send_disconnect(r2chan, cause);
		} else if (r2chan->r2_state == OR2_CLEAR_FWD_RXD){
			/* if the user want to hangup the call and the other end
			   already said they want too, then just report the call end event */
			report_call_end(r2chan);
		} else {
			/* this is a normal clear backward */
			if (send_clear_backward(r2chan)) {
				openr2_log(r2chan, OR2_LOG_ERROR, "Failed to send Clear Backward!, cannot disconnect call nicely!, may be try again?\n");
				return -1;
			}
		}
	} else {
		if (send_clear_forward(r2chan)) {
			openr2_log(r2chan, OR2_LOG_ERROR, "Failed to send Clear Forward!, cannot disconnect call nicely! may be try again?\n");
			return -1;
		}
	}
	return 0;
}

const char *openr2_proto_get_category_string(openr2_calling_party_category_t category)
{
	switch (category) {
	case OR2_CALLING_PARTY_CATEGORY_NATIONAL_SUBSCRIBER:
		return "National Subscriber";
	case OR2_CALLING_PARTY_CATEGORY_NATIONAL_PRIORITY_SUBSCRIBER:
		return "National Priority Subscriber";
	case OR2_CALLING_PARTY_CATEGORY_INTERNATIONAL_SUBSCRIBER:
		return "International Subscriber";
	case OR2_CALLING_PARTY_CATEGORY_INTERNATIONAL_PRIORITY_SUBSCRIBER:
		return "International Priority Subscriber";
	default:
		return "*Unknown*";
	}
}

openr2_calling_party_category_t openr2_proto_get_category(const char *category)
{
	if (!strncasecmp(category, "NATIONAL_SUBSCRIBER", sizeof("NATIONAL_SUBSCRIBER")-1)) {
		return OR2_CALLING_PARTY_CATEGORY_NATIONAL_SUBSCRIBER;
	} else if (!strncasecmp(category, "NATIONAL_PRIORITY_SUBSCRIBER", sizeof("NATIONAL_PRIORITY_SUBSCRIBER")-1)) {
		return OR2_CALLING_PARTY_CATEGORY_NATIONAL_PRIORITY_SUBSCRIBER;
	} else if (!strncasecmp(category, "INTERNATIONAL_SUBSCRIBER", sizeof("INTERNATIONAL_SUBSCRIBER")-1)) {
		return OR2_CALLING_PARTY_CATEGORY_INTERNATIONAL_SUBSCRIBER;
	} else if (!strncasecmp(category, "INTERNATIONAL_PRIORITY_SUBSCRIBER", sizeof("INTERNATIONAL_PRIORITY_SUBSCRIBER")-1)) {
		return OR2_CALLING_PARTY_CATEGORY_INTERNATIONAL_PRIORITY_SUBSCRIBER;
	}	
	return OR2_CALLING_PARTY_CATEGORY_UNKNOWN;
}

openr2_variant_t openr2_proto_get_variant(const char *variant_name)
{
	int i;
	int limit = sizeof(r2variants)/sizeof(r2variants[0]);
	for (i = 0; i < limit; i++) {
		if (!strncasecmp(r2variants[i].name, variant_name, sizeof(r2variants[i].name)-1)) {
			return r2variants[i].id;
		}
	}
	return OR2_VAR_UNKNOWN;
}

const char *openr2_proto_get_variant_string(openr2_variant_t variant)
{
	int i;
	int limit = sizeof(r2variants)/sizeof(r2variants[0]);
	for (i = 0; i < limit; i++) {
		if (variant == r2variants[i].id) {
			return r2variants[i].name;
		}
	}
	return "*UNKNOWN*";
}

const char *openr2_proto_get_state(openr2_chan_t *r2chan, int tx)
{
	OR2_CHAN_STACK;
	/* Since the state does not depend on ABCD bits only (some of them are repated)
	   we should fix this. Currently only works to detect IDLE,BLOCK conditions, but
	   even those could be false-detected */
	int i;
	openr2_abcd_signal_t signal = tx ? r2chan->abcd_write : r2chan->abcd_read;
	for (i = 0; i < sizeof(r2chan->r2context->abcd_signals)/sizeof(r2chan->r2context->abcd_signals[0]); i++) {
		if (r2chan->r2context->abcd_signals[i] == signal) {
			return abcd_names[i];
		}
		
	}
	return "*Unknown*";
}
const char *openr2_proto_get_rx_state_string(openr2_chan_t *r2chan)
{
	return openr2_proto_get_state(r2chan, 0);
}

const char *openr2_proto_get_tx_state_string(openr2_chan_t *r2chan)
{
	return openr2_proto_get_state(r2chan, 1);
}

const char *openr2_proto_get_call_state_string(openr2_chan_t *r2chan)
{
	return callstate2str(r2chan->call_state);
}

const char *openr2_proto_get_r2_state_string(openr2_chan_t *r2chan)
{
	return r2state2str(r2chan->r2_state);
}
const char *openr2_proto_get_mf_state_string(openr2_chan_t *r2chan)
{
	return mfstate2str(r2chan->mf_state);
}

const char *openr2_proto_get_mf_group_string(openr2_chan_t *r2chan)
{
	return mfgroup2str(r2chan->mf_group);
}

const char *openr2_proto_get_call_mode_string(openr2_call_mode_t mode)
{
	return get_string_from_mode(mode);
}

int openr2_proto_get_mf_tx(openr2_chan_t *r2chan)
{
	return r2chan->mf_write_tone;
}

int openr2_proto_get_mf_rx(openr2_chan_t *r2chan)
{
	return r2chan->mf_read_tone;
}



