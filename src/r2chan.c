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
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include "openr2/r2hwcompat.h"
#include "openr2/r2context.h"
#include "openr2/r2log.h"
#include "openr2/r2proto.h"
#include "openr2/r2utils.h"
#include "openr2/r2chan.h"

static openr2_chan_t *__openr2_chan_new_from_fd(openr2_context_t *r2context, int chanfd, void *mf_write_handle, void *mf_read_handle, int fdcreated)
{
	int res, zapval, channo;
	unsigned i;
	openr2_chan_t *r2chan = NULL;
	OR2_HW_GAINS chan_gains;
	OR2_HW_BUFFER_INFO chan_buffers;
	OR2_HW_PARAMS chan_params;
#ifdef OR2_MF_DEBUG
	char logfile[1024];
#endif
	res = ioctl(chanfd, OR2_HW_OP_CHANNO, &channo);
	if (res) {
		r2context->last_error = OR2_LIBERR_SYSCALL_FAILED;
		openr2_log2(r2context, OR2_LOG_ERROR, "Failed to get channel number from descriptor %d (%s)\n", chanfd, strerror(errno));
		if (fdcreated) {
			close(chanfd);
		}
		return NULL;
	}
	/* let's check the signaling */
	res = ioctl(chanfd, OR2_HW_OP_GET_PARAMS, &chan_params);
	if (res) {
		r2context->last_error = OR2_LIBERR_SYSCALL_FAILED;
		openr2_log2(r2context, OR2_LOG_ERROR, "Failed to get signaling information for channel %d (%s)\n", channo, strerror(errno));
		if (fdcreated) {
			close(chanfd);
		}	
		return NULL;
	}
	if (OR2_HW_SIG_CAS != chan_params.sigtype) {
		r2context->last_error = OR2_LIBERR_INVALID_CHAN_SIGNALING;
		openr2_log2(r2context, OR2_LOG_ERROR, "chan %d does not has CAS signaling\n", channo);
		if (fdcreated) {
			close(chanfd);
		}
		return NULL;
	}

	/* setup buffers and gains  */
	res = ioctl(chanfd, OR2_HW_OP_GET_BUFINFO, &chan_buffers);
	if (res) {
		r2context->last_error = OR2_LIBERR_SYSCALL_FAILED;
		openr2_log2(r2context, OR2_LOG_ERROR, "Failed to retrieve buffer information for chan %d (%s)\n", 
			    channo, strerror(errno));
		if (fdcreated) {
			close(chanfd);
		}	
		return NULL;
	}
	chan_buffers.txbufpolicy = OR2_HW_POLICY_IMMEDIATE;
	chan_buffers.rxbufpolicy = OR2_HW_POLICY_IMMEDIATE;
	chan_buffers.numbufs = 4;
	chan_buffers.bufsize = OR2_CHAN_READ_SIZE;
	res = ioctl(chanfd, OR2_HW_OP_SET_BUFINFO, &chan_buffers);
	if (res) {
		r2context->last_error = OR2_LIBERR_SYSCALL_FAILED;
		openr2_log2(r2context, OR2_LOG_ERROR, "Failed to set buffer information for chan %d (%s)\n", channo, strerror(errno));
		if (fdcreated) {
			close(chanfd);
		}	
		return NULL;
	}

	chan_gains.chan = 0;
	for (i = 0; i < 256; i++) {
		chan_gains.rxgain[i] = chan_gains.txgain[i] = i;
	}

	res = ioctl(chanfd, OR2_HW_OP_SET_GAINS, &chan_gains);
	if (res) {
		r2context->last_error = OR2_LIBERR_SYSCALL_FAILED;
		openr2_log2(r2context, OR2_LOG_ERROR, "Failed to set gains on channel %d (%s)\n", channo, strerror(errno));
		if (fdcreated) {
			close(chanfd);
		}
		return NULL;
	}

	zapval = OR2_HW_LAW_ALAW;
	res = ioctl(chanfd, OR2_HW_OP_SET_LAW, &zapval);
	if (res) {
		r2context->last_error = OR2_LIBERR_SYSCALL_FAILED;
		openr2_log2(r2context, OR2_LOG_ERROR, "Failed to set ALAW codec on channel %d (%s)\n", channo, strerror(errno));
		if (fdcreated) {
			close(chanfd);
		}	
		return NULL;
	}

	zapval = 0;
	res = ioctl(chanfd, OR2_HW_OP_SET_ECHO_CANCEL, &zapval);
	if (res) {
		r2context->last_error = OR2_LIBERR_SYSCALL_FAILED;
		openr2_log2(r2context, OR2_LOG_ERROR, "Failed to put echo-cancel off on channel %d (%s)\n", channo, strerror(errno));
		if (fdcreated) {
			close(chanfd);
		}	
		return NULL;
	}

	/* zap channel setup has succeeded, now allocate the R2 channel*/
	r2chan = calloc(1, sizeof(*r2chan));
	if (!r2chan) {
		r2context->last_error = OR2_LIBERR_SYSCALL_FAILED;
		openr2_log2(r2context, OR2_LOG_ERROR, "Failed to allocate memory for r2chan %d\n", channo);
		if (fdcreated) {
			close(chanfd);
		}	
		return NULL;
	}

#ifdef OR2_MF_DEBUG
	/* open the channel log */
	snprintf(logfile, sizeof(logfile)-1, "openr2-chan-%d-tx.raw", channo);
	logfile[sizeof(logfile)-1] = 0;
	r2chan->mf_write_fd = open(logfile, O_CREAT | O_TRUNC | O_WRONLY, 0666);
	if (-1 == r2chan->mf_read_fd) {
		r2context->last_error = OR2_LIBERR_SYSCALL_FAILED;
		openr2_log2(r2context, OR2_LOG_ERROR, "Failed to open MF-tx debug file %s for chan %d: %s\n", logfile, strerror(errno), channo);
		if (fdcreated) {
			close(chanfd);
		}	
		free(r2chan);
		return NULL;
	}
	snprintf(logfile, sizeof(logfile)-1, "openr2-chan-%d-rx.raw", channo);
	logfile[sizeof(logfile)-1] = 0;
	r2chan->mf_read_fd = open(logfile, O_CREAT | O_TRUNC | O_WRONLY, 0666);
	if (-1 == r2chan->mf_read_fd) {
		r2context->last_error = OR2_LIBERR_SYSCALL_FAILED;
		openr2_log2(r2context, OR2_LOG_ERROR, "Failed to open MF-rx debug file %s for chan %d: %s\n", logfile, strerror(errno), channo);
		if (fdcreated) {
			close(chanfd);
		}	
		close(r2chan->mf_write_fd);
		free(r2chan);
		return NULL;
	}
#endif

	/* if we created the zap device we need to close it too */
	r2chan->fd_created = fdcreated;

	/* channel number ( zap device number ) */	
	r2chan->number = channo;

	/* channel fd */
	r2chan->fd = chanfd;

	/* zap buffer size to write data */
	r2chan->zap_buf_size = chan_buffers.bufsize;

	/* start with read enabled */
	r2chan->read_enabled = 1;

	/* set the owner context */
	r2chan->r2context = r2context;

	/* MF tone detection hooks handles */
	r2chan->mf_write_handle = mf_write_handle ? mf_write_handle : &r2chan->default_mf_write_handle;
	r2chan->mf_read_handle = mf_read_handle ? mf_read_handle : &r2chan->default_mf_read_handle;

	/* set default logger and default logging level */
	r2chan->on_channel_log = openr2_log_channel_default;

	/* set ABCD indicators to invalid */
	r2chan->abcd_rx_signal = OR2_ABCD_INVALID;
	r2chan->abcd_tx_signal = OR2_ABCD_INVALID;

	/* add ourselves to the queue of channels in the context */
	openr2_context_add_channel(r2context, r2chan);

	OR2_CHAN_STACK;
	return r2chan;
}

openr2_chan_t *openr2_chan_new(openr2_context_t *r2context, int channo, void *mf_write_handle, void *mf_read_handle)
{
	int chanfd, res;

	/* open the zap generic channel interface */
	chanfd = open(OR2_HW_CHANNEL_FILE_NAME, O_RDWR | O_NONBLOCK);
	if (-1 == chanfd) {
		r2context->last_error = OR2_LIBERR_SYSCALL_FAILED;
		openr2_log2(r2context, OR2_LOG_ERROR, "Failed to open zap control device (%s)\n", strerror(errno));
		return NULL;
	}

	/* choose the requested channel */
	res = ioctl(chanfd, OR2_HW_OP_SPECIFY, &channo);
	if (res) {
		r2context->last_error = OR2_LIBERR_SYSCALL_FAILED;
		openr2_log2(r2context, OR2_LOG_ERROR, "Failed to choose channel %d (%s)\n", channo, strerror(errno));
		close(chanfd);
		return NULL;
	}
	return __openr2_chan_new_from_fd(r2context, chanfd, mf_write_handle, mf_read_handle, 1);
}

openr2_chan_t *openr2_chan_new_from_fd(openr2_context_t *r2context, int chanfd, void *mf_write_handle, void *mf_read_handle)
{
	return __openr2_chan_new_from_fd(r2context, chanfd, mf_write_handle, mf_read_handle, 0);
}

static int openr2_chan_handle_zap_event(openr2_chan_t *r2chan, int event)
{
	OR2_CHAN_STACK;
	switch (event) {
	case OR2_HW_EVENT_BITS_CHANGED:
		openr2_proto_handle_abcd_change(r2chan);
		break;
	case OR2_HW_EVENT_ALARM:
	case OR2_HW_EVENT_NO_ALARM:
		openr2_log(r2chan, OR2_LOG_DEBUG, (event == OR2_HW_EVENT_ALARM) ? "Alarm Raised\n" : "Alarm Cleared\n");
		EMI(r2chan)->on_hardware_alarm(r2chan, ((event == OR2_HW_EVENT_NO_ALARM) ? 1 : 0));
		break;
	default:
		openr2_log(r2chan, OR2_LOG_DEBUG, "Unhandled hardware event %d\n", event);
		break;
	}
	return 0;
}

int openr2_chan_process_event(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	openr2_callback_t callback;
	void *data;
	struct timeval nowtv;
	int interesting_events, events, res, tone_result, wrote;
	unsigned i;
	uint8_t read_buf[OR2_CHAN_READ_SIZE];
	int16_t tone_buf[OR2_CHAN_READ_SIZE];
	res = gettimeofday(&nowtv, NULL);
	if ( res == -1 ) {
		openr2_log(r2chan, OR2_LOG_ERROR, "Yikes! gettimeofday failed!\n");
	}
	/* handle any scheduled timer */
	if ( res != -1 && r2chan->sched_timer.callback && timercmp(&r2chan->sched_timer.time, &nowtv, <=) ) {
		callback = r2chan->sched_timer.callback;
		data = r2chan->sched_timer.data;
		openr2_chan_cancel_timer(r2chan);
		openr2_log(r2chan, OR2_LOG_DEBUG, "calling callback on chan %d\n", r2chan->number);
		callback(r2chan, data);
	}

	while(1) {

		/* we're always interested in ABCD bit events and we don't want not block */
		interesting_events = OR2_HW_IO_MUX_SIG_EVENT | OR2_HW_IO_MUX_NO_WAIT;

		/* we also want to be notified about read-ready if we have read enabled */
		if (r2chan->read_enabled) {
			interesting_events |= OR2_HW_IO_MUX_READ;
		}

		/* we also want to be notified about write-ready if we're in the MF process and have some tone to write */
		if (OR2_MF_OFF_STATE != r2chan->mf_state && MFI(r2chan)->mf_want_generate(r2chan->mf_write_handle, r2chan->mf_write_tone) ) {
			interesting_events |= OR2_HW_IO_MUX_WRITE;
		}

		res = ioctl(r2chan->fd, OR2_HW_OP_IO_MUX, &interesting_events);
		if (res) {
			EMI(r2chan)->on_os_error(r2chan, errno);
			return -1;
		}	
		/* if there is no interesting events, do nothing */
		if (!interesting_events) {
			return -1;
		}
		/* if there is a signaling eventset, probably ABCD bits just changed */
		if (OR2_HW_IO_MUX_SIG_EVENT & interesting_events) {
			res = ioctl(r2chan->fd, OR2_HW_OP_GET_EVENT, &events);
			if ( !res && events ) {
				openr2_chan_handle_zap_event(r2chan, events);
			}
			continue;
		}
		if (OR2_HW_IO_MUX_READ & interesting_events) {
			res = read(r2chan->fd, read_buf, sizeof(read_buf));
			if (-1 == res) {
				EMI(r2chan)->on_os_error(r2chan, errno);
				return -1;
			}
			/* if the MF detector is enabled, we are supposed to detect tones */
			if (r2chan->mf_state != OR2_MF_OFF_STATE) {
				/* assuming ALAW codec */
				for (i = 0; i < res; i++) {
					tone_buf[i] = TI(r2chan)->alaw_to_linear(read_buf[i]);
				}	
#ifdef OR2_MF_DEBUG	
				write(r2chan->mf_read_fd, tone_buf, res*2);
#endif
				tone_result = MFI(r2chan)->mf_detect_tone(r2chan->mf_read_handle, tone_buf, res);
				if ( tone_result != -1 ) {
					openr2_proto_handle_mf_tone(r2chan, tone_result);
				} 
			} else if (r2chan->answered) {
				EMI(r2chan)->on_call_read(r2chan, read_buf, res);
			}
			continue;
		}
		/* we only write tones here. Speech write is responsibility of the user, he should call
		   openr2_chan_write for that */
		if (OR2_HW_IO_MUX_WRITE & interesting_events) {
			res = MFI(r2chan)->mf_generate_tone(r2chan->mf_write_handle, tone_buf, r2chan->zap_buf_size);
			/* if there are no samples to convert and write then continue,
			   the generate routine already took care of it */
			if (!res) {
				continue;
			}
			/* an error on tone generation, lets just bail out and hope for the best */
			if (-1 == res) {
				openr2_log(r2chan, OR2_LOG_ERROR, "Failed to generate MF tone.\n");
				return -1;
			}
#ifdef OR2_MF_DEBUG
			write(r2chan->mf_write_fd, tone_buf, res*2);
#endif
			for (i = 0; i < res; i++) {
				read_buf[i] = TI(r2chan)->linear_to_alaw(tone_buf[i]);
			}
			wrote = write(r2chan->fd, read_buf, res);
			if (wrote != res) {
				EMI(r2chan)->on_os_error(r2chan, errno);
			}
			continue;
		}
	}	
	return 0;
}

void openr2_chan_set_timer(openr2_chan_t *r2chan, int ms, openr2_callback_t callback, void *cb_data)
{
	OR2_CHAN_STACK;
	struct timeval tv;
	int res;
	res = gettimeofday(&tv, NULL);
	if ( -1 == res ) {
		perror("Failed to get time of day!!");
		return;
	}
	r2chan->sched_timer.time.tv_sec = tv.tv_sec + ( ms / 1000 );
	r2chan->sched_timer.time.tv_usec = tv.tv_usec + ( ms % 1000) * 1000;
	/* more than 1000000 microseconds, then increment one second */
	 if ( r2chan->sched_timer.time.tv_usec > 1000000 ) {
		 r2chan->sched_timer.time.tv_sec += 1;
		 r2chan->sched_timer.time.tv_usec -= 1000000;
	}
	r2chan->sched_timer.callback = callback;
	r2chan->sched_timer.data = cb_data;
}

void openr2_chan_cancel_timer(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	r2chan->sched_timer.callback = NULL;
	r2chan->sched_timer.data = NULL;
	timerclear(&r2chan->sched_timer.time);
}

void openr2_chan_delete(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	if (MFI(r2chan)->mf_read_dispose) {
		MFI(r2chan)->mf_read_dispose(r2chan->mf_read_handle);
	}	
	if (MFI(r2chan)->mf_write_dispose) {
		MFI(r2chan)->mf_write_dispose(r2chan->mf_write_handle);
	}	
	if (r2chan->fd_created) {
		close(r2chan->fd);
	}	
	if (r2chan->logfile) {
		fclose(r2chan->logfile);
	}
#ifdef OR2_MF_DEBUG
	close(r2chan->mf_write_fd);
	close(r2chan->mf_read_fd);
#endif
	free(r2chan);
}

int openr2_chan_accept_call(openr2_chan_t *r2chan, openr2_call_mode_t mode)
{
	OR2_CHAN_STACK;
	return openr2_proto_accept_call(r2chan, mode);
}

int openr2_chan_answer_call(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	return openr2_proto_answer_call(r2chan);
}

int openr2_chan_disconnect_call(openr2_chan_t *r2chan, openr2_call_disconnect_cause_t cause)
{
	OR2_CHAN_STACK;
	return openr2_proto_disconnect_call(r2chan, cause);
}

int openr2_chan_write(openr2_chan_t *r2chan, const unsigned char *buf, int buf_size)
{
	OR2_CHAN_STACK;
	int res = 0;
	int wrote = 0;
	while (wrote < buf_size) {
		res = write(r2chan->fd, buf, buf_size);
		if (res == -1 && errno != EAGAIN) {
			EMI(r2chan)->on_os_error(r2chan, errno);
			break;
		}
		wrote += res;
	}	
	return wrote;
}

int openr2_chan_make_call(openr2_chan_t *r2chan, const char *ani, const char *dnid, openr2_calling_party_category_t category)
{
	OR2_CHAN_STACK;
	return openr2_proto_make_call(r2chan, ani, dnid, category);
}

openr2_direction_t openr2_chan_get_direction(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	return r2chan->direction;
}

void openr2_chan_set_logging_func(openr2_chan_t *r2chan, openr2_logging_func_t logcallback)
{
	OR2_CHAN_STACK;
	r2chan->on_channel_log = logcallback;
}

int openr2_chan_get_fd(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	return r2chan->fd;
}

int openr2_chan_get_number(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	return r2chan->number;
}

openr2_context_t *openr2_chan_get_context(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	return r2chan->r2context;
}

void openr2_chan_set_client_data(openr2_chan_t *r2chan, void *data)
{
	OR2_CHAN_STACK;
	r2chan->client_data = data;
}

void *openr2_chan_get_client_data(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	return r2chan->client_data;
}

int openr2_chan_get_time_to_next_event(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	int res, ms;
	struct timeval currtime;
	res = gettimeofday(&currtime, NULL);
	if (-1 == res) {
		return -1;
	}
	/* if no callback then there is no timer active */
	if (!r2chan->sched_timer.callback) {
		return -1;
	}
	ms = ( ( ( r2chan->sched_timer.time.tv_sec - currtime.tv_sec   ) * 1000 ) + 
	       ( ( r2chan->sched_timer.time.tv_usec - currtime.tv_usec ) / 1000 ) );
	if ( ms < 0 )
		return 0;
	return ms;
}

openr2_log_level_t openr2_chan_set_log_level(openr2_chan_t *r2chan, openr2_log_level_t level)
{
	OR2_CHAN_STACK;
	openr2_log_level_t retlevel = r2chan->loglevel;
	r2chan->loglevel = level;
	return retlevel;
}

openr2_log_level_t openr2_chan_get_log_level(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	return r2chan->loglevel;
}

void openr2_chan_enable_read(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	r2chan->read_enabled = 1;
}

void openr2_chan_disable_read(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	r2chan->read_enabled = 0;
}

int openr2_chan_get_read_enabled(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	return r2chan->read_enabled;
}

void openr2_chan_enable_call_files(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	r2chan->call_files = 1;
}

void openr2_chan_disable_call_files(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	r2chan->call_files = 0;
}

int openr2_chan_get_call_files_enabled(openr2_chan_t *r2chan)
{
	OR2_CHAN_STACK;
	return r2chan->call_files;
}

const char *openr2_chan_get_dnis(openr2_chan_t *r2chan)
{
	return r2chan->dnis;
}

const char *openr2_chan_get_ani(openr2_chan_t *r2chan)
{
	return r2chan->ani;
}


