/*
 *  Simple MPEG parser to achieve network/service information.
 *
 *  refered standards:
 *
 *    ETSI EN 300 468
 *    ETSI TR 101 211
 *    ETSI ETR 211
 *    ITU-T H.222.0
 *
 * 2005-05-10 - Basic ATSC PSIP parsing support added
 *    ATSC Standard Revision B (A65/B)
 *
 * Thanks to Sean Device from Triveni for providing access to ATSC signals
 *    and to Kevin Fowlks for his independent ATSC scanning tool.
 *
 * Please contribute: It is possible that some descriptors for ATSC are
 *        not parsed yet and thus the result won't be complete.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <assert.h>
#include <glob.h>
#include <ctype.h>
#include <iconv.h>
#include <langinfo.h>

#include <linux/dvb/frontend.h>
#include <linux/dvb/dmx.h>

#include "list.h"
#include "diseqc.h"
#include "dump-zap.h"
#include "dump-vdr.h"
#include "scan.h"
#include "lnb.h"

#include "atsc_psip_section.h"

static char demux_devname[80];

static struct dvb_frontend_info fe_info = {
	.type = -1
};

int verbosity = 2;

static int long_timeout;
static int current_tp_only;
static int no_ATSC_PSIP;
static int ATSC_type=1;
static int vdr_dump_channum;
static int save_channel_info = 1; 
static int scan_play_video = 1; 

static struct lnb_types_st lnb_type;

static int rf_chan; 
static int fe_fd; 
static char description[25] = "Living room"; 

char *default_charset = "ISO-6937";
char *output_charset;
#define CS_OPTIONS "//TRANSLIT"

static enum fe_spectral_inversion spectral_inversion = INVERSION_AUTO;

enum table_type {
	PAT,
	PMT,
	SDT,
	NIT
};

enum format {
        OUTPUT_ZAP,
        OUTPUT_VDR,
	OUTPUT_PIDS
};
static enum format output_format = OUTPUT_ZAP;
static int output_format_set = 0;


enum polarisation {
	POLARISATION_HORIZONTAL     = 0x00,
	POLARISATION_VERTICAL       = 0x01,
	POLARISATION_CIRCULAR_LEFT  = 0x02,
	POLARISATION_CIRCULAR_RIGHT = 0x03
};

enum running_mode {
	RM_NOT_RUNNING = 0x01,
	RM_STARTS_SOON = 0x02,
	RM_PAUSING     = 0x03,
	RM_RUNNING     = 0x04
};

#define AUDIO_CHAN_MAX (32)
#define CA_SYSTEM_ID_MAX (16)

struct service {
	struct list_head list;
	int transport_stream_id;
	int service_id;
	char *provider_name;
	char *service_name;
	uint16_t pmt_pid;
	uint16_t pcr_pid;
	uint16_t video_pid;
	uint16_t audio_pid[AUDIO_CHAN_MAX];
	char audio_lang[AUDIO_CHAN_MAX][4];
	int audio_num;
	uint16_t ca_id[CA_SYSTEM_ID_MAX];
	int ca_num;
	uint16_t teletext_pid;
	uint16_t subtitling_pid;
	uint16_t ac3_pid;
	unsigned int type         : 8;
	unsigned int scrambled	  : 1;
	enum running_mode running;
	void *priv;
	int channel_num;
};

struct transponder {
	struct list_head list;
	struct list_head services;
	int network_id;
	int original_network_id;
	int transport_stream_id;
	enum fe_type type;
	struct dvb_frontend_parameters param;
	enum polarisation polarisation;		/* only for DVB-S */
	int orbital_pos;			/* only for DVB-S */
	unsigned int we_flag		  : 1;	/* West/East Flag - only for DVB-S */
	unsigned int scan_done		  : 1;
	unsigned int last_tuning_failed	  : 1;
	unsigned int other_frequency_flag : 1;	/* DVB-T */
	unsigned int wrong_frequency	  : 1;	/* DVB-T with other_frequency_flag */
	int n_other_f;
	uint32_t *other_f;			/* DVB-T freqeuency-list descriptor */
};


struct section_buf {
	struct list_head list;
	const char *dmx_devname;
	unsigned int run_once  : 1;
	unsigned int segmented : 1;	/* segmented by table_id_ext */
	int fd;
	int pid;
	int table_id;
	int table_id_ext;
	int section_version_number;
	uint8_t section_done[32];
	int sectionfilter_done;
	unsigned char buf[1024];
	time_t timeout;
	time_t start_time;
	time_t running_time;
	struct section_buf *next_seg;	/* this is used to handle
					 * segmented tables (like NIT-other)
					 */
};

struct virtual_channels {
	char vchan_name[20];
	int vchan_major_num; 
	int vchan_minor_num;
	int vchan_video_pid;
	int vchan_audio_pid;
};

struct channel_info {
	int chan_num;
	int chan_freq;
	int lock_status;
	int16_t rssi_dBm;
	float snr_dB;
	int ber;
	int uncorrected_blks;
	int num_vchans;
	struct virtual_channels vc[16]; 
};

static struct channel_info *pchan_info; 

static LIST_HEAD(scanned_transponders);
static LIST_HEAD(new_transponders);
static struct transponder *current_tp;


static void dump_dvb_parameters (FILE *f, struct transponder *t);

static void setup_filter (struct section_buf* s, const char *dmx_devname,
		          int pid, int tid, int tid_ext,
			  int run_once, int segmented, int timeout);
static void add_filter (struct section_buf *s);

static const char * fe_type2str(fe_type_t t);

static int atsc_chan_to_mhz(int chan);
static int atsc_mhz_to_chan(int freq_mhz); 
static void cleanup(void);
static void print_struct_buffers(void);
static void save_channel_info_file(FILE *fd);

/* According to the DVB standards, the combination of network_id and
 * transport_stream_id should be unique, but in real life the satellite
 * operators and broadcasters don't care enough to coordinate
 * the numbering. Thus we identify TPs by frequency (dvbscan handles only
 * one satellite at a time). Further complication: Different NITs on
 * one satellite sometimes list the same TP with slightly different
 * frequencies, so we have to search within some bandwidth.
 */
static struct transponder *alloc_transponder(uint32_t frequency)
{
	struct transponder *tp = calloc(1, sizeof(*tp));

	tp->param.frequency = frequency;
	INIT_LIST_HEAD(&tp->list);
	INIT_LIST_HEAD(&tp->services);
	list_add_tail(&tp->list, &new_transponders);
	return tp;
}

static int is_same_transponder(uint32_t f1, uint32_t f2)
{
	uint32_t diff;
	if (f1 == f2)
		return 1;
	diff = (f1 > f2) ? (f1 - f2) : (f2 - f1);
	//FIXME: use symbolrate etc. to estimate bandwidth
	if (diff < 2000) {
		debug("f1 = %u is same TP as f2 = %u\n", f1, f2);
		return 1;
	}
	return 0;
}

static struct transponder *find_transponder(uint32_t frequency)
{
	struct list_head *pos;
	struct transponder *tp;

	list_for_each(pos, &scanned_transponders) {
		tp = list_entry(pos, struct transponder, list);
		if (current_tp_only)
			return tp;
		if (is_same_transponder(tp->param.frequency, frequency))
			return tp;
	}
	list_for_each(pos, &new_transponders) {
		tp = list_entry(pos, struct transponder, list);
		if (is_same_transponder(tp->param.frequency, frequency))
			return tp;
	}
	return NULL;
}

static void copy_transponder(struct transponder *d, struct transponder *s)
{
	struct list_head *pos;
	struct service *service;

	if (d->transport_stream_id != s->transport_stream_id) {
		/* propagate change to any already allocated services */
		list_for_each(pos, &d->services) {
			service = list_entry(pos, struct service, list);
			service->transport_stream_id = s->transport_stream_id;
		}
	}

	d->network_id = s->network_id;
	d->original_network_id = s->original_network_id;
	d->transport_stream_id = s->transport_stream_id;
	d->type = s->type;
	memcpy(&d->param, &s->param, sizeof(d->param));
	d->polarisation = s->polarisation;
	d->orbital_pos = s->orbital_pos;
	d->we_flag = s->we_flag;
	d->scan_done = s->scan_done;
	d->last_tuning_failed = s->last_tuning_failed;
	d->other_frequency_flag = s->other_frequency_flag;
	d->n_other_f = s->n_other_f;
	if (d->n_other_f) {
		d->other_f = calloc(d->n_other_f, sizeof(uint32_t));
		memcpy(d->other_f, s->other_f, d->n_other_f * sizeof(uint32_t));
	}
	else
		d->other_f = NULL;
}

/* service_ids are guaranteed to be unique within one TP
 * (the DVB standards say theay should be unique within one
 * network, but in real life...)
 */
static struct service *alloc_service(struct transponder *tp, int service_id)
{
	struct service *s = calloc(1, sizeof(*s));
	INIT_LIST_HEAD(&s->list);
	s->service_id = service_id;
	s->transport_stream_id = tp->transport_stream_id;
	list_add_tail(&s->list, &tp->services);
	return s;
}

static struct service *find_service(struct transponder *tp, int service_id)
{
	struct list_head *pos;
	struct service *s;

	list_for_each(pos, &tp->services) {
		s = list_entry(pos, struct service, list);
		if (s->service_id == service_id)
			return s;
	}
	return NULL;
}


static void parse_ca_identifier_descriptor (const unsigned char *buf,
				     struct service *s)
{
	unsigned char len = buf [1];
	unsigned int i;

	buf += 2;

	if (len > sizeof(s->ca_id)) {
		len = sizeof(s->ca_id);
		warning("too many CA system ids\n");
	}
	memcpy(s->ca_id, buf, len);
	for (i = 0; i < len / sizeof(s->ca_id[0]); i++)
		moreverbose("  CA ID 0x%04x\n", s->ca_id[i]);
}


static void parse_iso639_language_descriptor (const unsigned char *buf, struct service *s)
{
	unsigned char len = buf [1];

	buf += 2;

	if (len >= 4) {
		debug("    LANG=%.3s %d\n", buf, buf[3]);
		memcpy(s->audio_lang[s->audio_num], buf, 3);
#if 0
		/* seems like the audio_type is wrong all over the place */
		//if (buf[3] == 0) -> normal
		if (buf[3] == 1)
			s->audio_lang[s->audio_num][3] = '!'; /* clean effects (no language) */
		else if (buf[3] == 2)
			s->audio_lang[s->audio_num][3] = '?'; /* for the hearing impaired */
		else if (buf[3] == 3)
			s->audio_lang[s->audio_num][3] = '+'; /* visually impaired commentary */
#endif
	}
}

static void parse_network_name_descriptor (const unsigned char *buf, void *dummy)
{
	(void)dummy;

	unsigned char len = buf [1];

	info("Network Name '%.*s'\n", len, buf + 2);
}

static void parse_terrestrial_uk_channel_number (const unsigned char *buf, void *dummy)
{
	(void)dummy;

	int i, n, channel_num, service_id;
	struct list_head *p1, *p2;
	struct transponder *t;
	struct service *s;

	// 32 bits per record
	n = buf[1] / 4;
	if (n < 1)
		return;

	// desc id, desc len, (service id, service number)
	buf += 2;
	for (i = 0; i < n; i++) {
		service_id = (buf[0]<<8)|(buf[1]&0xff);
		channel_num = ((buf[2]&0x03)<<8)|(buf[3]&0xff);
		debug("Service ID 0x%x has channel number %d ", service_id, channel_num);
		list_for_each(p1, &scanned_transponders) {
			t = list_entry(p1, struct transponder, list);
			list_for_each(p2, &t->services) {
				s = list_entry(p2, struct service, list);
				if (s->service_id == service_id)
					s->channel_num = channel_num;
			}
		}
		buf += 4;
	}
}

static long bcd32_to_cpu (const int b0, const int b1, const int b2, const int b3)
{
	return ((b0 >> 4) & 0x0f) * 10000000 + (b0 & 0x0f) * 1000000 +
	       ((b1 >> 4) & 0x0f) * 100000   + (b1 & 0x0f) * 10000 +
	       ((b2 >> 4) & 0x0f) * 1000     + (b2 & 0x0f) * 100 +
	       ((b3 >> 4) & 0x0f) * 10       + (b3 & 0x0f);
}


static const fe_code_rate_t fec_tab [8] = {
	FEC_AUTO, FEC_1_2, FEC_2_3, FEC_3_4,
	FEC_5_6, FEC_7_8, FEC_NONE, FEC_NONE
};


static const fe_modulation_t qam_tab [6] = {
	QAM_AUTO, QAM_16, QAM_32, QAM_64, QAM_128, QAM_256
};
 

static void parse_cable_delivery_system_descriptor (const unsigned char *buf,
					     struct transponder *t)
{
	if (!t) {
		warning("cable_delivery_system_descriptor outside transport stream definition (ignored)\n");
		return;
	}
	t->type = FE_QAM;

	t->param.frequency = bcd32_to_cpu (buf[2], buf[3], buf[4], buf[5]);
	t->param.frequency *= 100;
	t->param.u.qam.fec_inner = fec_tab[buf[12] & 0x07];
	t->param.u.qam.symbol_rate = 10 * bcd32_to_cpu (buf[9],
							buf[10],
							buf[11],
							buf[12] & 0xf0);
	if ((buf[8] & 0x0f) > 5)
		t->param.u.qam.modulation = QAM_AUTO;
	else
		t->param.u.qam.modulation = qam_tab[buf[8] & 0x0f];
	t->param.inversion = spectral_inversion;

	if (verbosity >= 5) {
		debug("%#04x/%#04x ", t->network_id, t->transport_stream_id);
		dump_dvb_parameters (stderr, t);
		if (t->scan_done)
			dprintf(5, " (done)");
		if (t->last_tuning_failed)
			dprintf(5, " (tuning failed)");
		dprintf(5, "\n");
	}
}


static void parse_satellite_delivery_system_descriptor (const unsigned char *buf,
						 struct transponder *t)
{
	if (!t) {
		warning("satellite_delivery_system_descriptor outside transport stream definition (ignored)\n");
		return;
	}
	t->type = FE_QPSK;
	t->param.frequency = 10 * bcd32_to_cpu (buf[2], buf[3], buf[4], buf[5]);
	t->param.u.qpsk.fec_inner = fec_tab[buf[12] & 0x07];
	t->param.u.qpsk.symbol_rate = 10 * bcd32_to_cpu (buf[9],
							 buf[10],
							 buf[11],
							 buf[12] & 0xf0);

	t->polarisation = (buf[8] >> 5) & 0x03;
	t->param.inversion = spectral_inversion;

	t->orbital_pos = bcd32_to_cpu (0x00, 0x00, buf[6], buf[7]);
	t->we_flag = buf[8] >> 7;

	if (verbosity >= 5) {
		debug("%#04x/%#04x ", t->network_id, t->transport_stream_id);
		dump_dvb_parameters (stderr, t);
		if (t->scan_done)
			dprintf(5, " (done)");
		if (t->last_tuning_failed)
			dprintf(5, " (tuning failed)");
		dprintf(5, "\n");
	}
}


static void parse_terrestrial_delivery_system_descriptor (const unsigned char *buf,
						   struct transponder *t)
{
	static const fe_modulation_t m_tab [] = { QPSK, QAM_16, QAM_64, QAM_AUTO };
	static const fe_code_rate_t ofec_tab [8] = { FEC_1_2, FEC_2_3, FEC_3_4,
					       FEC_5_6, FEC_7_8 };
	struct dvb_ofdm_parameters *o;

	if (!t) {
		warning("terrestrial_delivery_system_descriptor outside transport stream definition (ignored)\n");
		return;
	}
	o = &t->param.u.ofdm;
	t->type = FE_OFDM;

	t->param.frequency = (buf[2] << 24) | (buf[3] << 16);
	t->param.frequency |= (buf[4] << 8) | buf[5];
	t->param.frequency *= 10;
	t->param.inversion = spectral_inversion;

	o->bandwidth = BANDWIDTH_8_MHZ + ((buf[6] >> 5) & 0x3);
	o->constellation = m_tab[(buf[7] >> 6) & 0x3];
	o->hierarchy_information = HIERARCHY_NONE + ((buf[7] >> 3) & 0x3);

	if ((buf[7] & 0x7) > 4)
		o->code_rate_HP = FEC_AUTO;
	else
		o->code_rate_HP = ofec_tab [buf[7] & 0x7];

	if (((buf[8] >> 5) & 0x7) > 4)
		o->code_rate_LP = FEC_AUTO;
	else
		o->code_rate_LP = ofec_tab [(buf[8] >> 5) & 0x7];

	o->guard_interval = GUARD_INTERVAL_1_32 + ((buf[8] >> 3) & 0x3);

	o->transmission_mode = (buf[8] & 0x2) ?
			       TRANSMISSION_MODE_8K :
			       TRANSMISSION_MODE_2K;

	t->other_frequency_flag = (buf[8] & 0x01);

	if (verbosity >= 5) {
		debug("%#04x/%#04x ", t->network_id, t->transport_stream_id);
		dump_dvb_parameters (stderr, t);
		if (t->scan_done)
			dprintf(5, " (done)");
		if (t->last_tuning_failed)
			dprintf(5, " (tuning failed)");
		dprintf(5, "\n");
	}
}

static void parse_frequency_list_descriptor (const unsigned char *buf,
				      struct transponder *t)
{
	int n, i;
	typeof(*t->other_f) f;

	if (!t) {
		warning("frequency_list_descriptor outside transport stream definition (ignored)\n");
		return;
	}
	if (t->other_f)
		return;

	n = (buf[1] - 1) / 4;
	if (n < 1 || (buf[2] & 0x03) != 3)
		return;

	t->other_f = calloc(n, sizeof(*t->other_f));
	t->n_other_f = n;
	buf += 3;
	for (i = 0; i < n; i++) {
		f = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
		t->other_f[i] = f * 10;
		buf += 4;
	}
}

/*
 * handle character set correctly (e.g. via iconv)
 *   c.f. EN 300 468 annex A
 */

struct charset_conv {
	unsigned len;
	unsigned char  data[3];
};

/* This table is the Latin 00 table. Basically ISO-6937 + Euro sign */
struct charset_conv en300468_latin_00_to_utf8[256] = {
	[0x00] = { 1, {0x00, } },
	[0x01] = { 1, {0x01, } },
	[0x02] = { 1, {0x02, } },
	[0x03] = { 1, {0x03, } },
	[0x04] = { 1, {0x04, } },
	[0x05] = { 1, {0x05, } },
	[0x06] = { 1, {0x06, } },
	[0x07] = { 1, {0x07, } },
	[0x08] = { 1, {0x08, } },
	[0x09] = { 1, {0x09, } },
	[0x0a] = { 1, {0x0a, } },
	[0x0b] = { 1, {0x0b, } },
	[0x0c] = { 1, {0x0c, } },
	[0x0d] = { 1, {0x0d, } },
	[0x0e] = { 1, {0x0e, } },
	[0x0f] = { 1, {0x0f, } },
	[0x10] = { 1, {0x10, } },
	[0x11] = { 1, {0x11, } },
	[0x12] = { 1, {0x12, } },
	[0x13] = { 1, {0x13, } },
	[0x14] = { 1, {0x14, } },
	[0x15] = { 1, {0x15, } },
	[0x16] = { 1, {0x16, } },
	[0x17] = { 1, {0x17, } },
	[0x18] = { 1, {0x18, } },
	[0x19] = { 1, {0x19, } },
	[0x1a] = { 1, {0x1a, } },
	[0x1b] = { 1, {0x1b, } },
	[0x1c] = { 1, {0x1c, } },
	[0x1d] = { 1, {0x1d, } },
	[0x1e] = { 1, {0x1e, } },
	[0x1f] = { 1, {0x1f, } },
	[0x20] = { 1, {0x20, } },
	[0x21] = { 1, {0x21, } },
	[0x22] = { 1, {0x22, } },
	[0x23] = { 1, {0x23, } },
	[0x24] = { 1, {0x24, } },
	[0x25] = { 1, {0x25, } },
	[0x26] = { 1, {0x26, } },
	[0x27] = { 1, {0x27, } },
	[0x28] = { 1, {0x28, } },
	[0x29] = { 1, {0x29, } },
	[0x2a] = { 1, {0x2a, } },
	[0x2b] = { 1, {0x2b, } },
	[0x2c] = { 1, {0x2c, } },
	[0x2d] = { 1, {0x2d, } },
	[0x2e] = { 1, {0x2e, } },
	[0x2f] = { 1, {0x2f, } },
	[0x30] = { 1, {0x30, } },
	[0x31] = { 1, {0x31, } },
	[0x32] = { 1, {0x32, } },
	[0x33] = { 1, {0x33, } },
	[0x34] = { 1, {0x34, } },
	[0x35] = { 1, {0x35, } },
	[0x36] = { 1, {0x36, } },
	[0x37] = { 1, {0x37, } },
	[0x38] = { 1, {0x38, } },
	[0x39] = { 1, {0x39, } },
	[0x3a] = { 1, {0x3a, } },
	[0x3b] = { 1, {0x3b, } },
	[0x3c] = { 1, {0x3c, } },
	[0x3d] = { 1, {0x3d, } },
	[0x3e] = { 1, {0x3e, } },
	[0x3f] = { 1, {0x3f, } },
	[0x40] = { 1, {0x40, } },
	[0x41] = { 1, {0x41, } },
	[0x42] = { 1, {0x42, } },
	[0x43] = { 1, {0x43, } },
	[0x44] = { 1, {0x44, } },
	[0x45] = { 1, {0x45, } },
	[0x46] = { 1, {0x46, } },
	[0x47] = { 1, {0x47, } },
	[0x48] = { 1, {0x48, } },
	[0x49] = { 1, {0x49, } },
	[0x4a] = { 1, {0x4a, } },
	[0x4b] = { 1, {0x4b, } },
	[0x4c] = { 1, {0x4c, } },
	[0x4d] = { 1, {0x4d, } },
	[0x4e] = { 1, {0x4e, } },
	[0x4f] = { 1, {0x4f, } },
	[0x50] = { 1, {0x50, } },
	[0x51] = { 1, {0x51, } },
	[0x52] = { 1, {0x52, } },
	[0x53] = { 1, {0x53, } },
	[0x54] = { 1, {0x54, } },
	[0x55] = { 1, {0x55, } },
	[0x56] = { 1, {0x56, } },
	[0x57] = { 1, {0x57, } },
	[0x58] = { 1, {0x58, } },
	[0x59] = { 1, {0x59, } },
	[0x5a] = { 1, {0x5a, } },
	[0x5b] = { 1, {0x5b, } },
	[0x5c] = { 1, {0x5c, } },
	[0x5d] = { 1, {0x5d, } },
	[0x5e] = { 1, {0x5e, } },
	[0x5f] = { 1, {0x5f, } },
	[0x60] = { 1, {0x60, } },
	[0x61] = { 1, {0x61, } },
	[0x62] = { 1, {0x62, } },
	[0x63] = { 1, {0x63, } },
	[0x64] = { 1, {0x64, } },
	[0x65] = { 1, {0x65, } },
	[0x66] = { 1, {0x66, } },
	[0x67] = { 1, {0x67, } },
	[0x68] = { 1, {0x68, } },
	[0x69] = { 1, {0x69, } },
	[0x6a] = { 1, {0x6a, } },
	[0x6b] = { 1, {0x6b, } },
	[0x6c] = { 1, {0x6c, } },
	[0x6d] = { 1, {0x6d, } },
	[0x6e] = { 1, {0x6e, } },
	[0x6f] = { 1, {0x6f, } },
	[0x70] = { 1, {0x70, } },
	[0x71] = { 1, {0x71, } },
	[0x72] = { 1, {0x72, } },
	[0x73] = { 1, {0x73, } },
	[0x74] = { 1, {0x74, } },
	[0x75] = { 1, {0x75, } },
	[0x76] = { 1, {0x76, } },
	[0x77] = { 1, {0x77, } },
	[0x78] = { 1, {0x78, } },
	[0x79] = { 1, {0x79, } },
	[0x7a] = { 1, {0x7a, } },
	[0x7b] = { 1, {0x7b, } },
	[0x7c] = { 1, {0x7c, } },
	[0x7d] = { 1, {0x7d, } },
	[0x7e] = { 1, {0x7e, } },
	[0x7f] = { 1, {0x7f, } },
	[0x80] = { 2, {0xc2, 0x80, } },
	[0x81] = { 2, {0xc2, 0x81, } },
	[0x82] = { 2, {0xc2, 0x82, } },
	[0x83] = { 2, {0xc2, 0x83, } },
	[0x84] = { 2, {0xc2, 0x84, } },
	[0x85] = { 2, {0xc2, 0x85, } },
	[0x86] = { 2, {0xc2, 0x86, } },
	[0x87] = { 2, {0xc2, 0x87, } },
	[0x88] = { 2, {0xc2, 0x88, } },
	[0x89] = { 2, {0xc2, 0x89, } },
	[0x8a] = { 2, {0xc2, 0x8a, } },
	[0x8b] = { 2, {0xc2, 0x8b, } },
	[0x8c] = { 2, {0xc2, 0x8c, } },
	[0x8d] = { 2, {0xc2, 0x8d, } },
	[0x8e] = { 2, {0xc2, 0x8e, } },
	[0x8f] = { 2, {0xc2, 0x8f, } },
	[0x90] = { 2, {0xc2, 0x90, } },
	[0x91] = { 2, {0xc2, 0x91, } },
	[0x92] = { 2, {0xc2, 0x92, } },
	[0x93] = { 2, {0xc2, 0x93, } },
	[0x94] = { 2, {0xc2, 0x94, } },
	[0x95] = { 2, {0xc2, 0x95, } },
	[0x96] = { 2, {0xc2, 0x96, } },
	[0x97] = { 2, {0xc2, 0x97, } },
	[0x98] = { 2, {0xc2, 0x98, } },
	[0x99] = { 2, {0xc2, 0x99, } },
	[0x9a] = { 2, {0xc2, 0x9a, } },
	[0x9b] = { 2, {0xc2, 0x9b, } },
	[0x9c] = { 2, {0xc2, 0x9c, } },
	[0x9d] = { 2, {0xc2, 0x9d, } },
	[0x9e] = { 2, {0xc2, 0x9e, } },
	[0x9f] = { 2, {0xc2, 0x9f, } },
	[0xa0] = { 2, {0xc2, 0xa0, } },
	[0xa1] = { 2, {0xc2, 0xa1, } },
	[0xa2] = { 2, {0xc2, 0xa2, } },
	[0xa3] = { 2, {0xc2, 0xa3, } },
	[0xa4] = { 3, { 0xe2, 0x82, 0xac,} },		/* Euro sign. Addition over the ISO-6937 standard */
	[0xa5] = { 2, {0xc2, 0xa5, } },
	[0xa6] = { 0, {} },
	[0xa7] = { 2, {0xc2, 0xa7, } },
	[0xa8] = { 2, {0xc2, 0xa4, } },
	[0xa9] = { 3, {0xe2, 0x80, 0x98, } },
	[0xaa] = { 3, {0xe2, 0x80, 0x9c, } },
	[0xab] = { 2, {0xc2, 0xab, } },
	[0xac] = { 3, {0xe2, 0x86, 0x90, } },
	[0xad] = { 3, {0xe2, 0x86, 0x91, } },
	[0xae] = { 3, {0xe2, 0x86, 0x92, } },
	[0xaf] = { 3, {0xe2, 0x86, 0x93, } },
	[0xb0] = { 2, {0xc2, 0xb0, } },
	[0xb1] = { 2, {0xc2, 0xb1, } },
	[0xb2] = { 2, {0xc2, 0xb2, } },
	[0xb3] = { 2, {0xc2, 0xb3, } },
	[0xb4] = { 2, {0xc3, 0x97, } },
	[0xb5] = { 2, {0xc2, 0xb5, } },
	[0xb6] = { 2, {0xc2, 0xb6, } },
	[0xb7] = { 2, {0xc2, 0xb7, } },
	[0xb8] = { 2, {0xc3, 0xb7, } },
	[0xb9] = { 3, {0xe2, 0x80, 0x99, } },
	[0xba] = { 3, {0xe2, 0x80, 0x9d, } },
	[0xbb] = { 2, {0xc2, 0xbb, } },
	[0xbc] = { 2, {0xc2, 0xbc, } },
	[0xbd] = { 2, {0xc2, 0xbd, } },
	[0xbe] = { 2, {0xc2, 0xbe, } },
	[0xbf] = { 2, {0xc2, 0xbf, } },
	[0xc0] = { 0, {} },
	[0xc1] = { 0, {} },
	[0xc2] = { 0, {} },
	[0xc3] = { 0, {} },
	[0xc4] = { 0, {} },
	[0xc5] = { 0, {} },
	[0xc6] = { 0, {} },
	[0xc7] = { 0, {} },
	[0xc8] = { 0, {} },
	[0xc9] = { 0, {} },
	[0xca] = { 0, {} },
	[0xcb] = { 0, {} },
	[0xcc] = { 0, {} },
	[0xcd] = { 0, {} },
	[0xce] = { 0, {} },
	[0xcf] = { 0, {} },
	[0xd0] = { 3, {0xe2, 0x80, 0x94, } },
	[0xd1] = { 2, {0xc2, 0xb9, } },
	[0xd2] = { 2, {0xc2, 0xae, } },
	[0xd3] = { 2, {0xc2, 0xa9, } },
	[0xd4] = { 3, {0xe2, 0x84, 0xa2, } },
	[0xd5] = { 3, {0xe2, 0x99, 0xaa, } },
	[0xd6] = { 2, {0xc2, 0xac, } },
	[0xd7] = { 2, {0xc2, 0xa6, } },
	[0xd8] = { 0, {} },
	[0xd9] = { 0, {} },
	[0xda] = { 0, {} },
	[0xdb] = { 0, {} },
	[0xdc] = { 3, {0xe2, 0x85, 0x9b, } },
	[0xdd] = { 3, {0xe2, 0x85, 0x9c, } },
	[0xde] = { 3, {0xe2, 0x85, 0x9d, } },
	[0xdf] = { 3, {0xe2, 0x85, 0x9e, } },
	[0xe0] = { 3, {0xe2, 0x84, 0xa6, } },
	[0xe1] = { 2, {0xc3, 0x86, } },
	[0xe2] = { 2, {0xc3, 0x90, } },
	[0xe3] = { 2, {0xc2, 0xaa, } },
	[0xe4] = { 2, {0xc4, 0xa6, } },
	[0xe5] = { 0, {} },
	[0xe6] = { 2, {0xc4, 0xb2, } },
	[0xe7] = { 2, {0xc4, 0xbf, } },
	[0xe8] = { 2, {0xc5, 0x81, } },
	[0xe9] = { 2, {0xc3, 0x98, } },
	[0xea] = { 2, {0xc5, 0x92, } },
	[0xeb] = { 2, {0xc2, 0xba, } },
	[0xec] = { 2, {0xc3, 0x9e, } },
	[0xed] = { 2, {0xc5, 0xa6, } },
	[0xee] = { 2, {0xc5, 0x8a, } },
	[0xef] = { 2, {0xc5, 0x89, } },
	[0xf0] = { 2, {0xc4, 0xb8, } },
	[0xf1] = { 2, {0xc3, 0xa6, } },
	[0xf2] = { 2, {0xc4, 0x91, } },
	[0xf3] = { 2, {0xc3, 0xb0, } },
	[0xf4] = { 2, {0xc4, 0xa7, } },
	[0xf5] = { 2, {0xc4, 0xb1, } },
	[0xf6] = { 2, {0xc4, 0xb3, } },
	[0xf7] = { 2, {0xc5, 0x80, } },
	[0xf8] = { 2, {0xc5, 0x82, } },
	[0xf9] = { 2, {0xc3, 0xb8, } },
	[0xfa] = { 2, {0xc5, 0x93, } },
	[0xfb] = { 2, {0xc3, 0x9f, } },
	[0xfc] = { 2, {0xc3, 0xbe, } },
	[0xfd] = { 2, {0xc5, 0xa7, } },
	[0xfe] = { 2, {0xc5, 0x8b, } },
	[0xff] = { 2, {0xc2, 0xad, } },
};

static void descriptorcpy(char **dest, const unsigned char *src, size_t len)
{
	size_t destlen, i;
	char *p, *type = default_charset;
	unsigned char *tmp = NULL;
	const unsigned char *s;
	int emphasis = 0, need_conversion = 1;

	if (*dest) {
		free (*dest);
		*dest = NULL;
	}
	if (!len)
		return;

	if (*src < 0x20) {
		switch (*src) {
		case 0x00:	type = "ISO-6937";		break;
		case 0x01:	type = "ISO-8859-5";		break;
		case 0x02:	type = "ISO-8859-6";		break;
		case 0x03:	type = "ISO-8859-7";		break;
		case 0x04:	type = "ISO-8859-8";		break;
		case 0x05:	type = "ISO-8859-9";		break;
		case 0x06:	type = "ISO-8859-10";		break;
		case 0x07:	type = "ISO-8859-11";		break;
		case 0x09:	type = "ISO-8859-13";		break;
		case 0x0a:	type = "ISO-8859-14";		break;
		case 0x0b:	type = "ISO-8859-15";		break;
		case 0x11:	type = "ISO-10646";		break;
		case 0x12:	type = "ISO-2022-KR";		break;
		case 0x13:	type = "GB2312";		break;
		case 0x14:	type = "BIG5";			break;
		case 0x15:	type = "ISO-10646/UTF-8";	break;
		case 0x10: /* ISO8859 */
			if ((*(src + 1) != 0) || *(src + 2) > 0x0f)
				break;
			src+=2;
			len-=2;
			switch(*src) {
			case 0x01:	type = "ISO-8859-1";		break;
			case 0x02:	type = "ISO-8859-2";		break;
			case 0x03:	type = "ISO-8859-3";		break;
			case 0x04:	type = "ISO-8859-4";		break;
			case 0x05:	type = "ISO-8859-5";		break;
			case 0x06:	type = "ISO-8859-6";		break;
			case 0x07:	type = "ISO-8859-7";		break;
			case 0x08:	type = "ISO-8859-8";		break;
			case 0x09:	type = "ISO-8859-9";		break;
			case 0x0a:	type = "ISO-8859-10";		break;
			case 0x0b:	type = "ISO-8859-11";		break;
			case 0x0d:	type = "ISO-8859-13";		break;
			case 0x0e:	type = "ISO-8859-14";		break;
			case 0x0f:	type = "ISO-8859-15";		break;
			}
		}
		src++;
		len--;
	}

	/*
	 * Destination length should be bigger. As the worse case seems to
	 * use 3 chars for one code, use it for destlen
	 */
	destlen = len * 3;
	*dest = malloc(destlen + 1);

	/* Remove special chars */
	if (!strncasecmp(type, "ISO-8859", 8) || !strcasecmp(type, "ISO-6937")) {
		/*
		 * Handles the ISO/IEC 10646 1-byte control codes
		 * (EN 300 468 v1.11.1 Table A.1)
		 * Emphasis will be represented as: *emphased*
		 */
		tmp = malloc(len + 2);
		p = (char *)tmp;
		s = src;
		for (i = 0; i < len; i++, s++) {
			if (*s >= 0x20 && (*s < 0x80 || *s > 0x9f))
				*p++ = *s;
			else if (*s == 0x86) {
				*p++ = '*';
				emphasis = 1;
			} else if (*s == 0x87 && emphasis) {
				*p++ = '*';
				emphasis = 0;
			}
		}
		if (emphasis)
			*p++ = '*';
		*p = '\0';
	} else {
		/*
		 * FIXME: need to handle the ISO/IEC 10646 2-byte control codes
		 * (EN 300 468 v1.11.1 Table A.2)
		 */
	}

	if (tmp)
		s = tmp;
	else
		s = src;

	p = *dest;
	if (!strcasecmp(type, "ISO-6937")) {
		unsigned char *p1, *p2;

		/* Convert charset to UTF-8 using Code table 00 - Latin */
		for (p1 = (unsigned char *)s; p1 < s + len; p1++)
			for (p2 = en300468_latin_00_to_utf8[*p1].data;
			     p2 < en300468_latin_00_to_utf8[*p1].data + en300468_latin_00_to_utf8[*p1].len;
			     p2++)
				*p++ = *p2;
		*p = '\0';

		/* If desired charset is not UTF-8, prepare for conversion */
		if (strcasecmp(output_charset, "UTF-8")) {
			if (tmp)
				free(tmp);
			tmp = (unsigned char *)*dest;
			len = p - *dest;

			*dest = malloc(destlen + 1);
			type = "UTF-8";
			s = tmp;
		} else
			need_conversion = 0;

	}

	/* Convert from original charset to the desired one */
	if (need_conversion) {
		char out_cs[strlen(output_charset) + 1 + sizeof(CS_OPTIONS)];

		p = *dest;
		strcpy(out_cs, output_charset);
		strcat(out_cs, CS_OPTIONS);

		iconv_t cd = iconv_open(out_cs, type);
		if (cd == (iconv_t)(-1)) {
			memcpy(p, s, len);
			p[len] = '\0';
			warning("Conversion from %s to %s not supported\n",
				 type, output_charset);
		} else {
			iconv(cd, (char **)&s, &len, &p, &destlen);
			iconv_close(cd);
			*p = '\0';
		}
	}

	if (tmp)
		free(tmp);
}

static void parse_service_descriptor (const unsigned char *buf, struct service *s)
{
	unsigned char len;

	s->type = buf[2];

	buf += 3;
	len = *buf;
	buf++;
	descriptorcpy(&s->provider_name, buf, len);

	buf += len;
	len = *buf;
	buf++;
	descriptorcpy(&s->service_name, buf, len);

	info("0x%04x 0x%04x: pmt_pid 0x%04x %s -- %s (%s%s)\n",
	    s->transport_stream_id,
	    s->service_id,
	    s->pmt_pid,
	    s->provider_name, s->service_name,
	    s->running == RM_NOT_RUNNING ? "not running" :
	    s->running == RM_STARTS_SOON ? "starts soon" :
	    s->running == RM_PAUSING     ? "pausing" :
	    s->running == RM_RUNNING     ? "running" : "???",
	    s->scrambled ? ", scrambled" : "");
}

static int find_descriptor(uint8_t tag, const unsigned char *buf,
		int descriptors_loop_len,
		const unsigned char **desc, int *desc_len)
{
	while (descriptors_loop_len > 0) {
		unsigned char descriptor_tag = buf[0];
		unsigned char descriptor_len = buf[1] + 2;

		if (!descriptor_len) {
			warning("descriptor_tag == 0x%02x, len is 0\n", descriptor_tag);
			break;
		}

		if (tag == descriptor_tag) {
			if (desc)
				*desc = buf;
			if (desc_len)
				*desc_len = descriptor_len;
			return 1;
		}

		buf += descriptor_len;
		descriptors_loop_len -= descriptor_len;
	}
	return 0;
}

static void parse_descriptors(enum table_type t, const unsigned char *buf,
			      int descriptors_loop_len, void *data)
{
	while (descriptors_loop_len > 0) {
		unsigned char descriptor_tag = buf[0];
		unsigned char descriptor_len = buf[1] + 2;

		if (!descriptor_len) {
			warning("descriptor_tag == 0x%02x, len is 0\n", descriptor_tag);
			break;
		}

		switch (descriptor_tag) {
		case 0x0a:
			if (t == PMT)
				parse_iso639_language_descriptor (buf, data);
			break;

		case 0x40:
			if (t == NIT)
				parse_network_name_descriptor (buf, data);
			break;

		case 0x43:
			if (t == NIT)
				parse_satellite_delivery_system_descriptor (buf, data);
			break; 

		case 0x44:
			if (t == NIT)
				parse_cable_delivery_system_descriptor (buf, data);
			break;

		case 0x48:
			if (t == SDT)
				parse_service_descriptor (buf, data);
			break;

		case 0x53:
			if (t == SDT)
				parse_ca_identifier_descriptor (buf, data);
			break;

		case 0x5a:
			if (t == NIT)
				parse_terrestrial_delivery_system_descriptor (buf, data);
			break;

		case 0x62:
			if (t == NIT)
				parse_frequency_list_descriptor (buf, data);
			break;

		case 0x83:
			/* 0x83 is in the privately defined range of descriptor tags,
			 * so we parse this only if the user says so to avoid
			 * problems when 0x83 is something entirely different... */
			if (t == NIT && vdr_dump_channum)
				parse_terrestrial_uk_channel_number (buf, data);
			break;
		
		default:
			verbosedebug("skip descriptor 0x%02x\n", descriptor_tag);
		};

		buf += descriptor_len;
		descriptors_loop_len -= descriptor_len;
	}
}


static void parse_pat(const unsigned char *buf, int section_length,
		      int transport_stream_id)
{
	(void)transport_stream_id;

	while (section_length > 0) {
		struct service *s;
		int service_id = (buf[0] << 8) | buf[1];

		if (service_id == 0)
			goto skip;	/* nit pid entry */

		/* SDT might have been parsed first... */
		s = find_service(current_tp, service_id);
		if (!s)
			s = alloc_service(current_tp, service_id);
		s->pmt_pid = ((buf[2] & 0x1f) << 8) | buf[3];
		if (!s->priv && s->pmt_pid) {
			s->priv = malloc(sizeof(struct section_buf));
			setup_filter(s->priv, demux_devname,
				     s->pmt_pid, 0x02, s->service_id, 1, 0, 5);

			add_filter (s->priv);
		}

skip:
		buf += 4;
		section_length -= 4;
	};
}


static void parse_pmt (const unsigned char *buf, int section_length, int service_id)
{
	int program_info_len;
	struct service *s;
        char msg_buf[14 * AUDIO_CHAN_MAX + 1];
        char *tmp;
        int i;

	s = find_service (current_tp, service_id);
	if (!s) {
		error("PMT for serivce_id 0x%04x was not in PAT\n", service_id);
		return;
	}

	s->pcr_pid = ((buf[0] & 0x1f) << 8) | buf[1];

	program_info_len = ((buf[2] & 0x0f) << 8) | buf[3];

	buf += program_info_len + 4;
	section_length -= program_info_len + 4;

	while (section_length >= 5) {
		int ES_info_len = ((buf[3] & 0x0f) << 8) | buf[4];
		int elementary_pid = ((buf[1] & 0x1f) << 8) | buf[2];

		switch (buf[0]) {
		case 0x01:
		case 0x02:
		case 0x1b: /* H.264 video stream */
			moreverbose("  VIDEO     : PID 0x%04x\n", elementary_pid);
			if (s->video_pid == 0)
				s->video_pid = elementary_pid;
			break;
		case 0x03:
		case 0x81: /* Audio per ATSC A/53B [2] Annex B */
		case 0x0f: /* ADTS Audio Stream - usually AAC */
		case 0x11: /* ISO/IEC 14496-3 Audio with LATM transport */
		case 0x04:
			moreverbose("  AUDIO     : PID 0x%04x\n", elementary_pid);
			if (s->audio_num < AUDIO_CHAN_MAX) {
				s->audio_pid[s->audio_num] = elementary_pid;
				parse_descriptors (PMT, buf + 5, ES_info_len, s);
				s->audio_num++;
			}
			else
				warning("more than %i audio channels, truncating\n",
				     AUDIO_CHAN_MAX);
			break;
		case 0x07:
			moreverbose("  MHEG      : PID 0x%04x\n", elementary_pid);
			break;
		case 0x0B:
			moreverbose("  DSM-CC    : PID 0x%04x\n", elementary_pid);
			break;
		case 0x06:
			if (find_descriptor(0x56, buf + 5, ES_info_len, NULL, NULL)) {
				moreverbose("  TELETEXT  : PID 0x%04x\n", elementary_pid);
				s->teletext_pid = elementary_pid;
				break;
			}
			else if (find_descriptor(0x59, buf + 5, ES_info_len, NULL, NULL)) {
				/* Note: The subtitling descriptor can also signal
				 * teletext subtitling, but then the teletext descriptor
				 * will also be present; so we can be quite confident
				 * that we catch DVB subtitling streams only here, w/o
				 * parsing the descriptor. */
				moreverbose("  SUBTITLING: PID 0x%04x\n", elementary_pid);
				s->subtitling_pid = elementary_pid;
				break;
			}
			else if (find_descriptor(0x6a, buf + 5, ES_info_len, NULL, NULL)) {
				moreverbose("  AC3       : PID 0x%04x\n", elementary_pid);
				s->ac3_pid = elementary_pid;
				break;
			}
			/* fall through */
		default:
			moreverbose("  OTHER     : PID 0x%04x TYPE 0x%02x\n", elementary_pid, buf[0]);
		};

		buf += ES_info_len + 5;
		section_length -= ES_info_len + 5;
	};


	tmp = msg_buf;
	tmp += sprintf(tmp, "0x%04x (%.4s)", s->audio_pid[0], s->audio_lang[0]);

	if (s->audio_num > AUDIO_CHAN_MAX) {
		warning("more than %i audio channels: %i, truncating to %i\n",
		      AUDIO_CHAN_MAX, s->audio_num, AUDIO_CHAN_MAX);
		s->audio_num = AUDIO_CHAN_MAX;
	}

        for (i=1; i<s->audio_num; i++)
                tmp += sprintf(tmp, ", 0x%04x (%.4s)", s->audio_pid[i], s->audio_lang[i]);

        debug("0x%04x 0x%04x: %s -- %s, pmt_pid 0x%04x, vpid 0x%04x, apid %s\n",
	    s->transport_stream_id,
	    s->service_id,
	    s->provider_name, s->service_name,
	    s->pmt_pid, s->video_pid, msg_buf);
}


static void parse_nit (const unsigned char *buf, int section_length, int network_id)
{
	int descriptors_loop_len = ((buf[0] & 0x0f) << 8) | buf[1];

	if (section_length < descriptors_loop_len + 4)
	{
		warning("section too short: network_id == 0x%04x, section_length == %i, "
		     "descriptors_loop_len == %i\n",
		     network_id, section_length, descriptors_loop_len);
		return;
	}

	parse_descriptors (NIT, buf + 2, descriptors_loop_len, NULL);

	section_length -= descriptors_loop_len + 4;
	buf += descriptors_loop_len + 4;

	while (section_length > 6) {
		int transport_stream_id = (buf[0] << 8) | buf[1];
		struct transponder *t, tn;

		descriptors_loop_len = ((buf[4] & 0x0f) << 8) | buf[5];

		if (section_length < descriptors_loop_len + 4)
		{
			warning("section too short: transport_stream_id == 0x%04x, "
			     "section_length == %i, descriptors_loop_len == %i\n",
			     transport_stream_id, section_length,
			     descriptors_loop_len);
			break;
		}

		debug("transport_stream_id 0x%04x\n", transport_stream_id);

		memset(&tn, 0, sizeof(tn));
		tn.type = -1;
		tn.network_id = network_id;
		tn.original_network_id = (buf[2] << 8) | buf[3];
		tn.transport_stream_id = transport_stream_id;

		parse_descriptors (NIT, buf + 6, descriptors_loop_len, &tn);

		if (tn.type == fe_info.type) {
			/* only add if develivery_descriptor matches FE type */
			t = find_transponder(tn.param.frequency);
			if (!t)
				t = alloc_transponder(tn.param.frequency);
			copy_transponder(t, &tn);
		}

		section_length -= descriptors_loop_len + 6;
		buf += descriptors_loop_len + 6;
	}
}


static void parse_sdt (const unsigned char *buf, int section_length,
		int transport_stream_id)
{
	(void)transport_stream_id;

	buf += 3;	       /*  skip original network id + reserved field */

	while (section_length >= 5) {
		int service_id = (buf[0] << 8) | buf[1];
		int descriptors_loop_len = ((buf[3] & 0x0f) << 8) | buf[4];
		struct service *s;

		if (section_length < descriptors_loop_len)
		{
			warning("section too short: service_id == 0x%02x, section_length == %i, "
			     "descriptors_loop_len == %i\n",
			     service_id, section_length,
			     descriptors_loop_len);
			break;
		}

		s = find_service(current_tp, service_id);
		if (!s)
			/* maybe PAT has not yet been parsed... */
			s = alloc_service(current_tp, service_id);

		s->running = (buf[3] >> 5) & 0x7;
		s->scrambled = (buf[3] >> 4) & 1;

		parse_descriptors (SDT, buf + 5, descriptors_loop_len, s);

		section_length -= descriptors_loop_len + 5;
		buf += descriptors_loop_len + 5;
	};
}

/* ATSC PSIP VCT */
static void parse_atsc_service_loc_desc(struct service *s,const unsigned char *buf)
{
	struct ATSC_service_location_descriptor d = read_ATSC_service_location_descriptor(buf);
	int i;
	unsigned char *b = (unsigned char *) buf+5;

	s->pcr_pid = d.PCR_PID;
	for (i=0; i < d.number_elements; i++) {
		struct ATSC_service_location_element e = read_ATSC_service_location_element(b);
		switch (e.stream_type) {
			case 0x02: /* video */
				s->video_pid = e.elementary_PID;
				moreverbose("  VIDEO     : PID 0x%04x\n", e.elementary_PID);
				break;
			case 0x81: /* ATSC audio */
				if (s->audio_num < AUDIO_CHAN_MAX) {
					s->audio_pid[s->audio_num] = e.elementary_PID;
					s->audio_lang[s->audio_num][0] = (e.ISO_639_language_code >> 16) & 0xff;
					s->audio_lang[s->audio_num][1] = (e.ISO_639_language_code >> 8)  & 0xff;
					s->audio_lang[s->audio_num][2] =  e.ISO_639_language_code        & 0xff;
					s->audio_num++;
				}
				moreverbose("  AUDIO     : PID 0x%04x lang: %s\n",e.elementary_PID,s->audio_lang[s->audio_num-1]);

				break;
			default:
				warning("unhandled stream_type: %x\n",e.stream_type);
				break;
		};
		b += 6;
	}
}

static void parse_atsc_ext_chan_name_desc(struct service *s,const unsigned char *buf)
{
	unsigned char *b = (unsigned char *) buf+2;
	int i,j;
	int num_str = b[0];

	b++;
	for (i = 0; i < num_str; i++) {
		int num_seg = b[3];
		b += 4; /* skip lang code */
		for (j = 0; j < num_seg; j++) {
			int comp_type = b[0],/* mode = b[1],*/ num_bytes = b[2];

			switch (comp_type) {
				case 0x00:
					if (s->service_name)
						free(s->service_name);
					s->service_name = malloc(num_bytes * sizeof(char) + 1);
					memcpy(s->service_name,&b[3],num_bytes);
					s->service_name[num_bytes] = '\0';
					break;
				default:
					warning("compressed strings are not supported yet\n");
					break;
			}
			b += 3 + num_bytes;
		}
	}
}

static void parse_psip_descriptors(struct service *s,const unsigned char *buf,int len)
{
	unsigned char *b = (unsigned char *) buf;
	int desc_len;
	while (len > 0) {
		desc_len = b[1];
		switch (b[0]) {
			case ATSC_SERVICE_LOCATION_DESCRIPTOR_ID:
				parse_atsc_service_loc_desc(s,b);
				break;
			case ATSC_EXTENDED_CHANNEL_NAME_DESCRIPTOR_ID:
				parse_atsc_ext_chan_name_desc(s,b);
				break;
			default:
				warning("unhandled psip descriptor: %02x\n",b[0]);
				break;
		}
		b += 2 + desc_len;
		len -= 2 + desc_len;
	}
}

static void parse_psip_vct (const unsigned char *buf, int section_length,
		int table_id, int transport_stream_id)
{
	(void)section_length;
	(void)table_id;
	(void)transport_stream_id;

/*	int protocol_version = buf[0];*/
	int num_channels_in_section = buf[1];
	int i;
	int pseudo_id = 0xffff;
	unsigned char *b = (unsigned char *) buf + 2;
	uint16_t snr;
	int16_t signal;
	uint32_t ber, uncorrected_blocks;
	fe_status_t status;
	int idx = rf_chan - 2;

	for (i = 0; i < num_channels_in_section; i++) {
		struct service *s;
		struct tvct_channel ch = read_tvct_channel(b);

		switch (ch.service_type) {
			case 0x01:
				info("analog channels won't be put info channels.conf\n");
				break;
			case 0x02: /* ATSC TV */
			case 0x03: /* ATSC Radio */
				break;
			case 0x04: /* ATSC Data */
			default:
				continue;
		}

		if (ch.program_number == 0)
			ch.program_number = --pseudo_id;

		s = find_service(current_tp, ch.program_number);
		if (!s)
			s = alloc_service(current_tp, ch.program_number);

		if (s->service_name)
			free(s->service_name);

		s->service_name = malloc(7*sizeof(unsigned char));
		/* TODO find a better solution to convert UTF-16 */
		s->service_name[0] = ch.short_name0;
		s->service_name[1] = ch.short_name1;
		s->service_name[2] = ch.short_name2;
		s->service_name[3] = ch.short_name3;
		s->service_name[4] = ch.short_name4;
		s->service_name[5] = ch.short_name5;
		s->service_name[6] = ch.short_name6;

		parse_psip_descriptors(s,&b[32],ch.descriptors_length);

		s->channel_num = ch.major_channel_number << 10 | ch.minor_channel_number;

		if (ch.hidden) {
			s->running = RM_NOT_RUNNING;
			info("service is not running, pseudo program_number.");
		} else {
			s->running = RM_RUNNING;
			info("service is running.");
		}

		if (save_channel_info) {
			
			if (i == 0) {
				if (ioctl(fe_fd, FE_READ_STATUS, &status) == -1) {
					errorn("FE_READ_STATUS failed");
					return ;
				}

				verbose(">>> tuning status == 0x%02x\n", status);

				if (ioctl(fe_fd, FE_READ_SIGNAL_STRENGTH, &signal) == -1)
					signal = -2;

				if (ioctl(fe_fd, FE_READ_SNR, &snr) == -1)
					snr = -2;

				if (ioctl(fe_fd, FE_READ_BER, &ber) == -1)
					ber = -2;

				if (ioctl(fe_fd, FE_READ_UNCORRECTED_BLOCKS, &uncorrected_blocks) == -1)
					uncorrected_blocks = -2;
			
				pchan_info[idx].chan_num = rf_chan;
				pchan_info[idx].chan_freq = atsc_chan_to_mhz(rf_chan);
				pchan_info[idx].snr_dB = (float) (snr / 10);
				pchan_info[idx].rssi_dBm = (int16_t) (signal / 100);
				pchan_info[idx].ber = ber;
				pchan_info[idx].uncorrected_blks = uncorrected_blocks;
				pchan_info[idx].lock_status = 0;
				pchan_info[idx].num_vchans = num_channels_in_section;
				
				if ( status & FE_HAS_LOCK) {
					pchan_info[idx].lock_status = 1; 	
					info("status %d | signal %d %04x | snr %d | ber %08x | unc %08x | ", 
					status, signal, signal, snr, ber, uncorrected_blocks);
				}
			}

			strcpy(pchan_info[idx].vc[i].vchan_name, s->service_name);
			pchan_info[idx].vc[i].vchan_major_num = ch.major_channel_number;
			pchan_info[idx].vc[i].vchan_minor_num = ch.minor_channel_number;
			pchan_info[idx].vc[i].vchan_video_pid = s->video_pid;
			pchan_info[idx].vc[i].vchan_audio_pid = s->audio_pid[0];	
		}	
	
		info("Channel number: %d:%d. Name: '%s'\n",
		ch.major_channel_number, ch.minor_channel_number,s->service_name);

		b += 32 + ch.descriptors_length;
	}
}

static int get_bit (uint8_t *bitfield, int bit)
{
	return (bitfield[bit/8] >> (bit % 8)) & 1;
}

static void set_bit (uint8_t *bitfield, int bit)
{
	bitfield[bit/8] |= 1 << (bit % 8);
}


/**
 *   returns 0 when more sections are expected
 *	   1 when all sections are read on this pid
 *	   -1 on invalid table id
 */
static int parse_section (struct section_buf *s)
{
	const unsigned char *buf = s->buf;
	int table_id;
	int section_length;
	int table_id_ext;
	int section_version_number;
	int section_number;
	int last_section_number;
	int i;

	table_id = buf[0];

	if (s->table_id != table_id)
		return -1;

	section_length = ((buf[1] & 0x0f) << 8) | buf[2];

	table_id_ext = (buf[3] << 8) | buf[4];
	section_version_number = (buf[5] >> 1) & 0x1f;
	section_number = buf[6];
	last_section_number = buf[7];

	if (s->segmented && s->table_id_ext != -1 && s->table_id_ext != table_id_ext) {
		/* find or allocate actual section_buf matching table_id_ext */
		while (s->next_seg) {
			s = s->next_seg;
			if (s->table_id_ext == table_id_ext)
				break;
		}
		if (s->table_id_ext != table_id_ext) {
			assert(s->next_seg == NULL);
			s->next_seg = calloc(1, sizeof(struct section_buf));
			s->next_seg->segmented = s->segmented;
			s->next_seg->run_once = s->run_once;
			s->next_seg->timeout = s->timeout;
			s = s->next_seg;
			s->table_id = table_id;
			s->table_id_ext = table_id_ext;
			s->section_version_number = section_version_number;
		}
	}

	if (s->section_version_number != section_version_number ||
			s->table_id_ext != table_id_ext) {
		struct section_buf *next_seg = s->next_seg;

		if (s->section_version_number != -1 && s->table_id_ext != -1)
			debug("section version_number or table_id_ext changed "
				"%d -> %d / %04x -> %04x\n",
				s->section_version_number, section_version_number,
				s->table_id_ext, table_id_ext);
		s->table_id_ext = table_id_ext;
		s->section_version_number = section_version_number;
		s->sectionfilter_done = 0;
		memset (s->section_done, 0, sizeof(s->section_done));
		s->next_seg = next_seg;
	}

	buf += 8;			/* past generic table header */
	section_length -= 5 + 4;	/* header + crc */
	if (section_length < 0) {
		warning("truncated section (PID 0x%04x, lenght %d)",
			s->pid, section_length + 9);
		return 0;
	}

	if (!get_bit(s->section_done, section_number)) {
		set_bit (s->section_done, section_number);

		debug("pid 0x%02x tid 0x%02x table_id_ext 0x%04x, "
		    "%i/%i (version %i)\n",
		    s->pid, table_id, table_id_ext, section_number,
		    last_section_number, section_version_number);

		switch (table_id) {
		case 0x00:
			verbose("PAT\n");
			parse_pat (buf, section_length, table_id_ext);
			break;

		case 0x02:
			verbose("PMT 0x%04x for service 0x%04x\n", s->pid, table_id_ext);
			parse_pmt (buf, section_length, table_id_ext);
			break;

		case 0x41:
			verbose("////////////////////////////////////////////// NIT other\n");
		case 0x40:
			verbose("NIT (%s TS)\n", table_id == 0x40 ? "actual":"other");
			parse_nit (buf, section_length, table_id_ext);
			break;

		case 0x42:
		case 0x46:
			verbose("SDT (%s TS)\n", table_id == 0x42 ? "actual":"other");
			parse_sdt (buf, section_length, table_id_ext);
			break;

		case 0xc8:
		case 0xc9:
			verbose("ATSC VCT\n");
			parse_psip_vct(buf, section_length, table_id, table_id_ext);
			break;
		default:
			;
		};

		for (i = 0; i <= last_section_number; i++)
			if (get_bit (s->section_done, i) == 0)
				break;

		if (i > last_section_number)
			s->sectionfilter_done = 1;
	}

	if (s->segmented) {
		/* always wait for timeout; this is because we don't now how
		 * many segments there are
		 */
		return 0;
	}
	else if (s->sectionfilter_done)
		return 1;

	return 0;
}


static int read_sections (struct section_buf *s)
{
	int section_length, count;

	if (s->sectionfilter_done && !s->segmented)
		return 1;

	/* the section filter API guarantess that we get one full section
	 * per read(), provided that the buffer is large enough (it is)
	 */
	if (((count = read (s->fd, s->buf, sizeof(s->buf))) < 0) && errno == EOVERFLOW)
		count = read (s->fd, s->buf, sizeof(s->buf));
	if (count < 0) {
		errorn("read_sections: read error");
		return -1;
	}

	if (count < 4)
		return -1;

	section_length = ((s->buf[1] & 0x0f) << 8) | s->buf[2];

	if (count != section_length + 3)
		return -1;

	if (parse_section(s) == 1)
		return 1;

	return 0;
}


static LIST_HEAD(running_filters);
static LIST_HEAD(waiting_filters);
static int n_running;
#define MAX_RUNNING 27
static struct pollfd poll_fds[MAX_RUNNING];
static struct section_buf* poll_section_bufs[MAX_RUNNING];


static void setup_filter (struct section_buf* s, const char *dmx_devname,
			  int pid, int tid, int tid_ext,
			  int run_once, int segmented, int timeout)
{
	memset (s, 0, sizeof(struct section_buf));

	s->fd = -1;
	s->dmx_devname = dmx_devname;
	s->pid = pid;
	s->table_id = tid;

	s->run_once = run_once;
	s->segmented = segmented;

	if (long_timeout)
		s->timeout = 5 * timeout;
	else
		s->timeout = timeout;

	s->table_id_ext = tid_ext;
	s->section_version_number = -1;

	INIT_LIST_HEAD (&s->list);
}

static void update_poll_fds(void)
{
	struct list_head *p;
	struct section_buf* s;
	int i;

	memset(poll_section_bufs, 0, sizeof(poll_section_bufs));
	for (i = 0; i < MAX_RUNNING; i++)
		poll_fds[i].fd = -1;
	i = 0;
	list_for_each (p, &running_filters) {
		if (i >= MAX_RUNNING)
			fatal("too many poll_fds\n");
		s = list_entry (p, struct section_buf, list);
		if (s->fd == -1)
			fatal("s->fd == -1 on running_filters\n");
		verbosedebug("poll fd %d\n", s->fd);
		poll_fds[i].fd = s->fd;
		poll_fds[i].events = POLLIN;
		poll_fds[i].revents = 0;
		poll_section_bufs[i] = s;
		i++;
	}
	if (i != n_running)
		fatal("n_running is hosed\n");
}

static int start_filter (struct section_buf* s)
{
	struct dmx_sct_filter_params f;

	if (n_running >= MAX_RUNNING)
		goto err0;
	if ((s->fd = open (s->dmx_devname, O_RDWR | O_NONBLOCK)) < 0)
		goto err0;

	verbosedebug("start filter pid 0x%04x table_id 0x%02x\n", s->pid, s->table_id);

	memset(&f, 0, sizeof(f));

	f.pid = (uint16_t) s->pid;

	if (s->table_id < 0x100 && s->table_id > 0) {
		f.filter.filter[0] = (uint8_t) s->table_id;
		f.filter.mask[0]   = 0xff;
	}
	if (s->table_id_ext < 0x10000 && s->table_id_ext > 0) {
		f.filter.filter[1] = (uint8_t) ((s->table_id_ext >> 8) & 0xff);
		f.filter.filter[2] = (uint8_t) (s->table_id_ext & 0xff);
		f.filter.mask[1] = 0xff;
		f.filter.mask[2] = 0xff;
	}

	f.timeout = 0;
	f.flags = DMX_IMMEDIATE_START | DMX_CHECK_CRC;

	if (ioctl(s->fd, DMX_SET_FILTER, &f) == -1) {
		errorn ("ioctl DMX_SET_FILTER failed");
		goto err1;
	}

	s->sectionfilter_done = 0;
	time(&s->start_time);

	list_del_init (&s->list);  /* might be in waiting filter list */
	list_add (&s->list, &running_filters);

	n_running++;
	update_poll_fds();

	return 0;

err1:
	ioctl (s->fd, DMX_STOP);
	close (s->fd);
err0:
	return -1;
}


static void stop_filter (struct section_buf *s)
{
	verbosedebug("stop filter pid 0x%04x\n", s->pid);
	ioctl (s->fd, DMX_STOP);
	close (s->fd);
	s->fd = -1;
	list_del (&s->list);
	s->running_time += time(NULL) - s->start_time;

	n_running--;
	update_poll_fds();
}


static void add_filter (struct section_buf *s)
{
	verbosedebug("add filter pid 0x%04x\n", s->pid);
	if (start_filter (s))
		list_add_tail (&s->list, &waiting_filters);
}


static void remove_filter (struct section_buf *s)
{
	verbosedebug("remove filter pid 0x%04x\n", s->pid);
	stop_filter (s);
	while (!list_empty(&waiting_filters)) {
		struct list_head *next = waiting_filters.next;
		s = list_entry (next, struct section_buf, list);
		if (start_filter (s))
			break;
	};
}


static void read_filters (void)
{
	struct section_buf *s;
	int i, n, done;

	n = poll(poll_fds, n_running, 1000);
	if (n == -1)
		errorn("poll");

	for (i = 0; i < n_running; i++) {
		s = poll_section_bufs[i];
		if (!s)
			fatal("poll_section_bufs[%d] is NULL\n", i);
		if (poll_fds[i].revents)
			done = read_sections (s) == 1;
		else
			done = 0; /* timeout */
		if (done || time(NULL) > s->start_time + s->timeout) {
			if (s->run_once) {
				if (done)
					verbosedebug("filter done pid 0x%04x\n", s->pid);
				else
					warning("filter timeout pid 0x%04x\n", s->pid);
				remove_filter (s);
			}
		}
	}
}


static int mem_is_zero (const void *mem, int size)
{
	const char *p = mem;
	int i;

	for (i=0; i<size; i++) {
		if (p[i] != 0x00)
			return 0;
	}

	return 1;
}


static int switch_pos = 0;

static int __tune_to_transponder (int frontend_fd, struct transponder *t)
{
	struct dvb_frontend_parameters p;
	fe_status_t s;
	current_tp = t;
	int i;

	if (mem_is_zero (&t->param, sizeof(struct dvb_frontend_parameters)))
		return -1;

	memcpy (&p, &t->param, sizeof(struct dvb_frontend_parameters));

	if (verbosity >= 1) {
		dprintf(1, ">>> tune to: ");
		dump_dvb_parameters (stderr, t);
		if (t->last_tuning_failed)
			dprintf(1, " (tuning failed)");
		dprintf(1, "\n");
	}

	if (ioctl(frontend_fd, FE_SET_FRONTEND, &p) == -1) {
		errorn("Setting frontend parameters failed");
		return -1;
	}

	rf_chan = atsc_mhz_to_chan(t->param.frequency/1000000);
	if (rf_chan < 0)
		info("Out of frequency Range: atsc_mhz_to_chan\n"); 
	
	for (i = 0; i < 10; i++) {
		usleep (200000);

		if (ioctl(frontend_fd, FE_READ_STATUS, &s) == -1) {
			errorn("FE_READ_STATUS failed");
			return -1;
		}

		if (s & FE_HAS_LOCK) {
			t->last_tuning_failed = 0;
			return 0;	
		}
	}

	warning(">>> tuning failed!!!\n");

	t->last_tuning_failed = 1;

	return -1;
}


static int set_delivery_system(int fd)
{
	struct dtv_properties props;
	struct dtv_property dvb_prop[1];

	dvb_prop[0].cmd = DTV_DELIVERY_SYSTEM;
	dvb_prop[0].u.data = SYS_ATSC;
	props.num = 1;
	props.props = dvb_prop;
	if (ioctl(fd, FE_SET_PROPERTY, &props) >= 0)
		return 0;
	return errno;
}

static int tune_to_transponder (int frontend_fd, struct transponder *t)
{
	int rc;

	/* move TP from "new" to "scanned" list */
	list_del_init(&t->list);
	list_add_tail(&t->list, &scanned_transponders);
	t->scan_done = 1;

	if (t->type != fe_info.type) {
		rc = set_delivery_system(frontend_fd);
		if (!rc)
			fe_info.type = t->type;
	}

	if (t->type != fe_info.type) {
		warning("frontend type (%s) is not compatible with requested tuning type (%s)\n",
				fe_type2str(fe_info.type),fe_type2str(t->type));
		/* ignore cable descriptors in sat NIT and vice versa */
		t->last_tuning_failed = 1;
		return -1;
	}

	if (__tune_to_transponder (frontend_fd, t) == 0)
		return 0;

	return __tune_to_transponder (frontend_fd, t);
}


static int tune_to_next_transponder (int frontend_fd)
{
	struct list_head *pos, *tmp;
	struct transponder *t, *to;
	uint32_t freq;

	list_for_each_safe(pos, tmp, &new_transponders) {
		
		t = list_entry (pos, struct transponder, list);
retry:
		if (tune_to_transponder (frontend_fd, t) == 0)
			return 0;
next:
		if (t->other_frequency_flag && t->other_f && t->n_other_f) {
			printf("\n insdie the if loop \n");
			/* check if the alternate freqeuncy is really new to us */
			freq = t->other_f[t->n_other_f - 1];
			t->n_other_f--;
			if (find_transponder(freq))
				goto next;

			/* remember tuning to the old frequency failed */
			to = calloc(1, sizeof(*to));
			to->param.frequency = t->param.frequency;
			to->wrong_frequency = 1;
			INIT_LIST_HEAD(&to->list);
			INIT_LIST_HEAD(&to->services);
			list_add_tail(&to->list, &scanned_transponders);
			copy_transponder(to, t);

			t->param.frequency = freq;
			info("retrying with f=%d\n", t->param.frequency);
			goto retry;
		}
	}
	return -1;
}

struct strtab {
	const char *str;
	int val;
};

static const char * enum2str(int v, const struct strtab *tab, const char *deflt)
{
	while (tab->str) {
		if (v == tab->val)
			return tab->str;
		tab++;
	}
	error("invalid enum value '%d'\n", v);
	return deflt;
}

static const char * fe_type2str(fe_type_t t)
{
	struct strtab typetab[] = {
		{ "QPSK", FE_QPSK,},
		{ "QAM",  FE_QAM, },
		{ "OFDM", FE_OFDM,},
		{ "ATSC", FE_ATSC,},
		{ NULL, 0 }
	};

	return enum2str(t, typetab, "UNK");
}

static int tune_initial (int frontend_fd)
{
	struct transponder *t;
	
	for (int chan = 2; chan < 52; ++chan) {	
		int32_t freq_hz = atsc_chan_to_mhz(chan) * 1000000;
		t = alloc_transponder(freq_hz);
		t->type = FE_ATSC;
		t->param.u.vsb.modulation = VSB_8;
	}

	return tune_to_next_transponder(frontend_fd);
	//return 0; 
}

static int atsc_chan_to_mhz(int chan)
{
	if (chan >= 2 && chan <= 4) {
		return 57 + (chan - 2) * 6;
	}
	else if (chan >= 5 && chan <= 6) {
		return 79 + (chan - 5) * 6;
	}
	else if (chan >= 7 && chan <= 13) {
		return 177 + (chan - 7) * 6;
	}
	else if (chan >=14 && chan <= 51) {
		return 473 + (chan - 14) * 6;
	}

	return -1;
}

static int atsc_mhz_to_chan(int freq_mhz)
{
	int freq = freq_mhz;

	if (freq >= 57 && freq <= 69) {
		return 2 + ( ( freq - 57) / 6); 
	}
	else if (freq >= 79 && freq <= 85) {
		return 5 + ( ( freq - 79) / 6);
	}
	else if (freq >= 177 && freq <= 213) {
		return 7 + ( ( freq - 177) / 6);
	}
	else if (freq >= 473 && freq <= 695) {
		return 14 + ( ( freq - 473) / 6);
	}

	return -1;
}

static void scan_tp_atsc(void)
{
	struct section_buf s0,s1,s2;

	if (no_ATSC_PSIP) {
		setup_filter(&s0, demux_devname, 0x00, 0x00, -1, 1, 0, 5); /* PAT */
		add_filter(&s0);
	} else {
		if (ATSC_type & 0x1) {
			setup_filter(&s0, demux_devname, 0x1ffb, 0xc8, -1, 1, 0, 5); /* terrestrial VCT */
			add_filter(&s0);
		}
		if (ATSC_type & 0x2) {
			setup_filter(&s1, demux_devname, 0x1ffb, 0xc9, -1, 1, 0, 5); /* cable VCT */
			add_filter(&s1);
		}
		setup_filter(&s2, demux_devname, 0x00, 0x00, -1, 1, 0, 5); /* PAT */
		add_filter(&s2);
	}

	do {
		read_filters ();
	} while (!(list_empty(&running_filters) &&
		   list_empty(&waiting_filters)));
}

static void scan_network (int frontend_fd)
{
	if (tune_initial (frontend_fd) < 0) {
		error("initial tuning failed\n");
		return;
	}

	do {
		scan_tp_atsc();
	} while (tune_to_next_transponder(frontend_fd) == 0);
}

static char sat_polarisation (struct transponder *t)
{
	return t->polarisation == POLARISATION_VERTICAL ? 'v' : 'h';
}

static int sat_number (struct transponder *t)
{
	(void) t;

	return switch_pos;
}

static void show_existing_tuning_data_files(void)
{
#ifndef DATADIR
#define DATADIR "/usr/local/share"
#endif
	static const char* prefixlist[] = { DATADIR "/dvb", "/etc/dvb",
					    DATADIR "/doc/packages/dvb", 0 };
	unsigned int i;
	const char **prefix;
	fprintf(stderr, "initial tuning data files:\n");
	for (prefix = prefixlist; *prefix; prefix++) {
		glob_t globbuf;
		char* globspec = malloc (strlen(*prefix)+9);
		strcpy (globspec, *prefix); strcat (globspec, "/dvb-?/*");
		if (! glob (globspec, 0, 0, &globbuf)) {
			for (i=0; i < globbuf.gl_pathc; i++)
				fprintf(stderr, " file: %s\n", globbuf.gl_pathv[i]);
		}
		free (globspec);
		globfree (&globbuf);
	}
}

static const char *usage = "\n"
	"usage: %s [options...] [-c | initial-tuning-data-file]\n"
	"	atsc/dvbscan doesn't do frequency scans, hence it needs initial\n"
	"	tuning data for at least one transponder/channel.\n"
	"	-c	scan on currently tuned transponder only\n"
	"	-a N	use DVB /dev/dvb/adapterN/\n"
	"	-f N	use DVB /dev/dvb/adapter?/frontendN\n"
	"	-d N	use DVB /dev/dvb/adapter?/demuxN\n"
	"	-5	multiply all filter timeouts by factor 5\n"
	"		for non-DVB-compliant section repitition rates\n"
	"	-u      UK DVB-T Freeview channel numbering for VDR\n\n"
	"	-P do not use ATSC PSIP tables for scanning\n"
	"	    (but only PAT and PMT) (applies for ATSC only)\n"
	"	-A N	check for ATSC 1=Terrestrial [default], 2=Cable or 3=both\n"
	"   -s save scanned channel information to a file\n"
	"	-l Antenna location (eg: Bedroom, living room, default: Living room)\n"
	"	-v scan and play the video (Each channel for about 5-10 seconds)\n"
	"Supported charsets by -C/-D parameters can be obtained via 'iconv -l' command\n";

void
bad_usage(char *pname, int problem)
{
	int i;
	struct lnb_types_st *lnbp;
	char **cp;

	switch (problem) {
	default:
	case 0:
		fprintf (stderr, usage, pname, output_charset);
		break;
	case 1:
		i = 0;
		fprintf(stderr, "-l <lnb-type> or -l low[,high[,switch]] in Mhz\n"
			"where <lnb-type> is:\n");
		while(NULL != (lnbp = lnb_enum(i))) {
			fprintf (stderr, "%s\n", lnbp->name);
			for (cp = lnbp->desc; *cp ; cp++) {
				fprintf (stderr, "   %s\n", *cp);
			}
			i++;
		}
		break;
	case 2:
		show_existing_tuning_data_files();
		fprintf (stderr, usage, pname);
	}
}

int main (int argc, char **argv)
{
	char frontend_devname [80]; // memset this buffer
	int adapter = 0, frontend = 0, demux = 0;
	int opt, i;
	int frontend_fd;
	int fe_open_mode;
	char *charset;
	FILE * chinfo_fd;

	/*
	 * Get the environment charset, and use it as the default
	 * output charset. In thesis, using nl_langinfo should be
	 * enough, but, in my tests, it is not as reliable as checking
	 * the environment vars directly.
	 */
	if ((charset = getenv("LC_ALL")) ||
	    (charset = getenv("LC_CTYPE")) ||
	    (charset = getenv ("LANG"))) {
		while (*charset != '.' && *charset)
			charset++;
		if (*charset == '.')
			charset++;
		if (*charset)
			output_charset = charset;
		else
			output_charset = nl_langinfo(CODESET);
	} else
		output_charset = nl_langinfo(CODESET);

	/* start with default lnb type */
	lnb_type = *lnb_enum(0);
	while ((opt = getopt(argc, argv, "a:c:d:f:5:u:P:A:s:v:l:")) != -1) {
		switch (opt) {
		case 'a':
			adapter = strtoul(optarg, NULL, 0);
			break;
		case 'c':
			current_tp_only = 1;
			if (!output_format_set)
				output_format = OUTPUT_PIDS;
			break;
		case 'd':
			demux = strtoul(optarg, NULL, 0);
			break;
		case 'f':
			frontend = strtoul(optarg, NULL, 0);
			break;
		case '5':
			long_timeout = 1;
			break;
		case 'u':
			vdr_dump_channum = 1;
			break; 
		case 'P':
			no_ATSC_PSIP = 1;
			break;
		case 'A':
			ATSC_type = strtoul(optarg,NULL,0);
			if (ATSC_type == 0 || ATSC_type > 3) {
				bad_usage(argv[0], 1);
				return -1;
			}
			break;
		case 's':
			save_channel_info = 1;
			break;
		case 'v':
			scan_play_video = 1;
			break;
		case 'l':
			strcpy(description, optarg);
			break;
		default:
			bad_usage(argv[0], 0);
			return -1;
			break;
		};
	}

	lnb_type.low_val *= 1000;	/* convert to kiloherz */
	lnb_type.high_val *= 1000;	/* convert to kiloherz */
	lnb_type.switch_val *= 1000;	/* convert to kiloherz */

	info("scanning \n");

	snprintf (frontend_devname, sizeof(frontend_devname),
		  "/dev/dvb/adapter%i/frontend%i", adapter, frontend);

	snprintf (demux_devname, sizeof(demux_devname),
		  "/dev/dvb/adapter%i/demux%i", adapter, demux);
	info("using '%s' and '%s'\n", frontend_devname, demux_devname);

	for (i = 0; i < MAX_RUNNING; i++)
		poll_fds[i].fd = -1;

	fe_open_mode = current_tp_only ? O_RDONLY : O_RDWR;
	if ((frontend_fd = open (frontend_devname, fe_open_mode)) < 0)
		fatal("failed to open '%s': %d %m\n", frontend_devname, errno);
	/* determine FE type and caps */
	if (ioctl(frontend_fd, FE_GET_INFO, &fe_info) == -1)
		fatal("FE_GET_INFO failed: %d %m\n", errno);

	if ((spectral_inversion == INVERSION_AUTO ) &&
	    !(fe_info.caps & FE_CAN_INVERSION_AUTO)) {
		info("Frontend can not do INVERSION_AUTO, trying INVERSION_OFF instead\n");
		spectral_inversion = INVERSION_OFF;
	}	

	if (save_channel_info) {	

		char str[50] = {0}; 

		sprintf(str, "%s_%s_%s.txt", __DATE__, __TIME__, description); 

		chinfo_fd = fopen((const char *) str, "w+");
		if (chinfo_fd == NULL) {
			printf("\n Unable to open a file: chinfo_fd \n");
			return -1; 
		}	

		pchan_info = (struct channel_info *) calloc( 50, sizeof(struct channel_info)); 
		
		if (pchan_info == NULL) {
			printf("MEMEORY NOT ALLOCATED: pchan_info \n");
			return -1;
		}

		fe_fd = frontend_fd;
	}

	if (current_tp_only) {
		current_tp = alloc_transponder(0); /* dummy */
		/* move TP from "new" to "scanned" list */
		list_del_init(&current_tp->list);
		list_add_tail(&current_tp->list, &scanned_transponders);
		current_tp->scan_done = 1;
		scan_tp_atsc();
	}
	else {
		scan_network (frontend_fd);
	}

	if (save_channel_info) {
		print_struct_buffers();
		save_channel_info_file(chinfo_fd);
	}

	if (scan_play_video) {
				
	}

	close (frontend_fd);
	
	cleanup();

	return 0;
}

static void save_channel_info_file(FILE *fd)
{

	int num_rf_chans = 0;
	int num_virtual_chans = 0;

	fprintf(fd, "%s %s\t\n",__DATE__, __TIME__);	
	fprintf(fd, "%s\t\n", description);
	fprintf(fd, "chan_num\tchan_Mhz\tlock_status\trssi[dBm]\tsnr[dB]\t");
	
	for (int i = 1; i < 17; ++i) {
		fprintf(fd, "vchan%d_num\tvchan%d_name\tvchan%d_video_pid\tvchan%d_audio_pid\t",
		i, i, i, i);
	}

	fprintf(fd, "\n");
	
	for (int j = 0; j < 50; ++j) {
	
		if (pchan_info[j].lock_status) {

			++num_rf_chans; 

			fprintf(fd, "%d\t%d\t%d\t%d\t%f\t",
			pchan_info[j].chan_num,
			pchan_info[j].chan_freq,
			pchan_info[j].lock_status,
			pchan_info[j].rssi_dBm,
			pchan_info[j].snr_dB);

			for (int z = 0; z < pchan_info[j].num_vchans; ++z) {
			
				++num_virtual_chans;

				fprintf(fd, "%d.%d\t%s\t%d\t%d\t",
				pchan_info[j].vc[z].vchan_major_num,
				pchan_info[j].vc[z].vchan_minor_num,
				pchan_info[j].vc[z].vchan_name,
				pchan_info[j].vc[z].vchan_video_pid,
				pchan_info[j].vc[z].vchan_audio_pid);
					
			}

			fprintf(fd, "\n");
		}
	}

	fprintf(fd, "Total Channels Locked\t%d RF channels\t%d Virtual channels\t\n",
	num_rf_chans, num_virtual_chans);
	
	fclose(fd);

}

static void print_struct_buffers(void)
{
	int i;
	
	for (i = 0; i < 50; ++ i) {
		
		printf("%d %d %f %d %d %d %d %d\n" ,pchan_info[i].chan_num,
		pchan_info[i].chan_freq,
		pchan_info[i].snr_dB,
		pchan_info[i].rssi_dBm,
		pchan_info[i].ber,
		pchan_info[i].uncorrected_blks,
		pchan_info[i].lock_status,
		pchan_info[i].num_vchans);
				

		for ( int j = 0; j < pchan_info[i].num_vchans; ++j)
		{
			printf("%s %d %d %d %d\n\n", pchan_info[i].vc[j].vchan_name,
			pchan_info[i].vc[j].vchan_major_num,
			pchan_info[i].vc[j].vchan_minor_num,
			pchan_info[i].vc[j].vchan_video_pid,
			pchan_info[i].vc[j].vchan_audio_pid);	
		}	
	}

}

static void cleanup()
{

	if (save_channel_info) {
		free(pchan_info);
	}
}

static void dump_dvb_parameters (FILE *f, struct transponder *t)
{
	switch (output_format) {
		case OUTPUT_PIDS:
		case OUTPUT_VDR:
			vdr_dump_dvb_parameters(f, t->type, &t->param,
					sat_polarisation (t), t->orbital_pos, t->we_flag);
			break;
		case OUTPUT_ZAP:
			zap_dump_dvb_parameters (f, t->type, &t->param,
					sat_polarisation (t), sat_number (t));
			break;
		default:
			break;
	}
}

