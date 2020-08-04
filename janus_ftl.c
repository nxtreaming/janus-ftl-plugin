/*
	This plugin is essentially a copy of janus_streaming by
	Lorenzo Miniero <lorenzo@meetecho.com>

	Modified to support the FTL protocol by
	Hayden McAfee <hayden@outlook.com> @ August of 2020
*/

#include <plugins/plugin.h>

#include <errno.h>
#include <netdb.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <jansson.h>

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#ifndef CURL_AT_LEAST_VERSION
#define CURL_AT_LEAST_VERSION(x,y,z) 0
#endif
#endif

#ifdef HAVE_LIBOGG
#include <ogg/ogg.h>
#endif

#include <debug.h>
#include <apierror.h>
#include <config.h>
#include <mutex.h>
#include <rtp.h>
#include <rtpsrtp.h>
#include <rtcp.h>
#include <record.h>
#include <utils.h>
#include <ip-utils.h>


/* Plugin information */
#define JANUS_STREAMING_VERSION			1
#define JANUS_STREAMING_VERSION_STRING	"0.0.1"
#define JANUS_STREAMING_DESCRIPTION		"This is an FTL streaming plugin for Janus, allowing WebRTC peers to watch/listen to media sent via FTL."
#define JANUS_STREAMING_NAME			"JANUS FTL plugin"
#define JANUS_STREAMING_AUTHOR			"Hayden McAfee"
#define JANUS_STREAMING_PACKAGE			"janus.plugin.ftl"

/* Plugin methods */
janus_plugin *create(void);
int janus_streaming_init(janus_callbacks *callback, const char *config_path);
void janus_streaming_destroy(void);
int janus_streaming_get_api_compatibility(void);
int janus_streaming_get_version(void);
const char *janus_streaming_get_version_string(void);
const char *janus_streaming_get_description(void);
const char *janus_streaming_get_name(void);
const char *janus_streaming_get_author(void);
const char *janus_streaming_get_package(void);
void janus_streaming_create_session(janus_plugin_session *handle, int *error);
struct janus_plugin_result *janus_streaming_handle_message(janus_plugin_session *handle, char *transaction, json_t *message, json_t *jsep);
json_t *janus_streaming_handle_admin_message(json_t *message);
void janus_streaming_setup_media(janus_plugin_session *handle);
void janus_streaming_incoming_rtp(janus_plugin_session *handle, janus_plugin_rtp *packet);
void janus_streaming_incoming_rtcp(janus_plugin_session *handle, janus_plugin_rtcp *packet);
void janus_streaming_data_ready(janus_plugin_session *handle);
void janus_streaming_hangup_media(janus_plugin_session *handle);
void janus_streaming_destroy_session(janus_plugin_session *handle, int *error);
json_t *janus_streaming_query_session(janus_plugin_session *handle);
static int janus_streaming_get_fd_port(int fd);

/* Plugin setup */
static janus_plugin janus_streaming_plugin =
	JANUS_PLUGIN_INIT (
		.init = janus_streaming_init,
		.destroy = janus_streaming_destroy,

		.get_api_compatibility = janus_streaming_get_api_compatibility,
		.get_version = janus_streaming_get_version,
		.get_version_string = janus_streaming_get_version_string,
		.get_description = janus_streaming_get_description,
		.get_name = janus_streaming_get_name,
		.get_author = janus_streaming_get_author,
		.get_package = janus_streaming_get_package,

		.create_session = janus_streaming_create_session,
		.handle_message = janus_streaming_handle_message,
		.handle_admin_message = janus_streaming_handle_admin_message,
		.setup_media = janus_streaming_setup_media,
		.incoming_rtp = janus_streaming_incoming_rtp,
		.incoming_rtcp = janus_streaming_incoming_rtcp,
		.data_ready = janus_streaming_data_ready,
		.hangup_media = janus_streaming_hangup_media,
		.destroy_session = janus_streaming_destroy_session,
		.query_session = janus_streaming_query_session,
	);

/* Plugin creator */
janus_plugin *create(void) {
	JANUS_LOG(LOG_VERB, "%s created!\n", JANUS_STREAMING_NAME);
	return &janus_streaming_plugin;
}

/* Parameter validation */
static struct janus_json_parameter request_parameters[] = {
	{"request", JSON_STRING, JANUS_JSON_PARAM_REQUIRED}
};
static struct janus_json_parameter id_parameters[] = {
	{"id", JSON_INTEGER, JANUS_JSON_PARAM_REQUIRED | JANUS_JSON_PARAM_POSITIVE}
};
static struct janus_json_parameter idopt_parameters[] = {
	{"id", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE}
};
static struct janus_json_parameter idstr_parameters[] = {
	{"id", JSON_STRING, JANUS_JSON_PARAM_REQUIRED}
};
static struct janus_json_parameter idstropt_parameters[] = {
	{"id", JSON_STRING, 0}
};
static struct janus_json_parameter watch_parameters[] = {
	{"pin", JSON_STRING, 0},
	{"offer_audio", JANUS_JSON_BOOL, 0},
	{"offer_video", JANUS_JSON_BOOL, 0},
	{"offer_data", JANUS_JSON_BOOL, 0},
	{"restart", JANUS_JSON_BOOL, 0}
};
static struct janus_json_parameter adminkey_parameters[] = {
	{"admin_key", JSON_STRING, JANUS_JSON_PARAM_REQUIRED}
};
static struct janus_json_parameter edit_parameters[] = {
	{"new_description", JSON_STRING, 0},
	{"new_secret", JSON_STRING, 0},
	{"new_pin", JSON_STRING, 0},
	{"new_is_private", JANUS_JSON_BOOL, 0},
	{"permanent", JANUS_JSON_BOOL, 0}
};
static struct janus_json_parameter create_parameters[] = {
	{"name", JSON_STRING, 0},
	{"description", JSON_STRING, 0},
	{"metadata", JSON_STRING, 0},
	{"is_private", JANUS_JSON_BOOL, 0},
	{"type", JSON_STRING, JANUS_JSON_PARAM_REQUIRED},
	{"secret", JSON_STRING, 0},
	{"pin", JSON_STRING, 0},
	{"audio", JANUS_JSON_BOOL, 0},
	{"video", JANUS_JSON_BOOL, 0},
	{"data", JANUS_JSON_BOOL, 0},
	{"permanent", JANUS_JSON_BOOL, 0}
};
static struct janus_json_parameter rtp_parameters[] = {
	{"collision", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"threads", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"srtpsuite", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"srtpcrypto", JSON_STRING, 0},
	{"e2ee", JANUS_JSON_BOOL, 0}
};
static struct janus_json_parameter live_parameters[] = {
	{"filename", JSON_STRING, JANUS_JSON_PARAM_REQUIRED},
	{"audiortpmap", JSON_STRING, 0},
	{"audiofmtp", JSON_STRING, 0},
	{"audiopt", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE}
};
static struct janus_json_parameter ondemand_parameters[] = {
	{"filename", JSON_STRING, JANUS_JSON_PARAM_REQUIRED},
	{"audiortpmap", JSON_STRING, 0},
	{"audiofmtp", JSON_STRING, 0},
	{"audiopt", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE}
};
#ifdef HAVE_LIBCURL
static struct janus_json_parameter rtsp_parameters[] = {
	{"url", JSON_STRING, 0},
	{"rtsp_user", JSON_STRING, 0},
	{"rtsp_pwd", JSON_STRING, 0},
	{"audiortpmap", JSON_STRING, 0},
	{"audiofmtp", JSON_STRING, 0},
	{"audiopt", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"videortpmap", JSON_STRING, 0},
	{"videofmtp", JSON_STRING, 0},
	{"videopt", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"videobufferkf", JANUS_JSON_BOOL, 0},
	{"rtspiface", JSON_STRING, 0},
	{"rtsp_failcheck", JANUS_JSON_BOOL, 0}
};
#endif
static struct janus_json_parameter rtp_audio_parameters[] = {
	{"audiomcast", JSON_STRING, 0},
	{"audioport", JSON_INTEGER, JANUS_JSON_PARAM_REQUIRED | JANUS_JSON_PARAM_POSITIVE},
	{"audiortcpport", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"audiopt", JSON_INTEGER, JANUS_JSON_PARAM_REQUIRED | JANUS_JSON_PARAM_POSITIVE},
	{"audiortpmap", JSON_STRING, JANUS_JSON_PARAM_REQUIRED},
	{"audiofmtp", JSON_STRING, 0},
	{"audioiface", JSON_STRING, 0},
	{"audioskew", JANUS_JSON_BOOL, 0}
};
static struct janus_json_parameter rtp_video_parameters[] = {
	{"videomcast", JSON_STRING, 0},
	{"videoport", JSON_INTEGER, JANUS_JSON_PARAM_REQUIRED | JANUS_JSON_PARAM_POSITIVE},
	{"videortcpport", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"videopt", JSON_INTEGER, JANUS_JSON_PARAM_REQUIRED | JANUS_JSON_PARAM_POSITIVE},
	{"videortpmap", JSON_STRING, JANUS_JSON_PARAM_REQUIRED},
	{"videofmtp", JSON_STRING, 0},
	{"videobufferkf", JANUS_JSON_BOOL, 0},
	{"videoiface", JSON_STRING, 0},
	{"videosimulcast", JANUS_JSON_BOOL, 0},
	{"videoport2", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"videoport3", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"videoskew", JANUS_JSON_BOOL, 0},
	{"videosvc", JANUS_JSON_BOOL, 0}
};
static struct janus_json_parameter rtp_data_parameters[] = {
	{"dataport", JSON_INTEGER, JANUS_JSON_PARAM_REQUIRED | JANUS_JSON_PARAM_POSITIVE},
	{"databuffermsg", JANUS_JSON_BOOL, 0},
	{"datatype", JSON_STRING, 0},
	{"dataiface", JSON_STRING, 0}
};
static struct janus_json_parameter destroy_parameters[] = {
	{"permanent", JANUS_JSON_BOOL, 0}
};
static struct janus_json_parameter recording_parameters[] = {
	{"action", JSON_STRING, JANUS_JSON_PARAM_REQUIRED}
};
static struct janus_json_parameter recording_start_parameters[] = {
	{"audio", JSON_STRING, 0},
	{"video", JSON_STRING, 0},
	{"data", JSON_STRING, 0}
};
static struct janus_json_parameter recording_stop_parameters[] = {
	{"audio", JANUS_JSON_BOOL, 0},
	{"video", JANUS_JSON_BOOL, 0},
	{"data", JANUS_JSON_BOOL, 0}
};
static struct janus_json_parameter simulcast_parameters[] = {
	{"substream", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"temporal", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"fallback", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE}
};
static struct janus_json_parameter svc_parameters[] = {
	{"spatial_layer", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"temporal_layer", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE}
};
static struct janus_json_parameter configure_parameters[] = {
	{"audio", JANUS_JSON_BOOL, 0},
	{"video", JANUS_JSON_BOOL, 0},
	{"data", JANUS_JSON_BOOL, 0},
	/* For VP8 (or H.264) simulcast */
	{"substream", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"temporal", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"fallback", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	/* For VP9 SVC */
	{"spatial_layer", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"temporal_layer", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE}
};
static struct janus_json_parameter disable_parameters[] = {
	{"stop_recording", JANUS_JSON_BOOL, 0}
};

/* Static configuration instance */
static janus_config *config = NULL;
static const char *config_folder = NULL;
static janus_mutex config_mutex = JANUS_MUTEX_INITIALIZER;

/* Useful stuff */
static volatile gint initialized = 0, stopping = 0;
static gboolean notify_events = TRUE;
static gboolean string_ids = FALSE;
static janus_callbacks *gateway = NULL;
static GThread *handler_thread;
static void *janus_streaming_handler(void *data);

/* RTP range to use for random ports */
#define DEFAULT_RTP_RANGE_MIN 10000
#define DEFAULT_RTP_RANGE_MAX 60000
static uint16_t rtp_range_min = DEFAULT_RTP_RANGE_MIN;
static uint16_t rtp_range_max = DEFAULT_RTP_RANGE_MAX;
static uint16_t rtp_range_slider = DEFAULT_RTP_RANGE_MIN;
static janus_mutex fd_mutex = JANUS_MUTEX_INITIALIZER;

static void *janus_streaming_ondemand_thread(void *data);
static void *janus_streaming_filesource_thread(void *data);
static void janus_streaming_relay_rtp_packet(gpointer data, gpointer user_data);
static void janus_streaming_relay_rtcp_packet(gpointer data, gpointer user_data);
static void *janus_streaming_relay_thread(void *data);
static void janus_streaming_hangup_media_internal(janus_plugin_session *handle);

typedef enum janus_streaming_type {
	janus_streaming_type_none = 0,
	janus_streaming_type_live,
	janus_streaming_type_on_demand,
} janus_streaming_type;

typedef enum janus_streaming_source {
	janus_streaming_source_none = 0,
	janus_streaming_source_file,
	janus_streaming_source_rtp,
} janus_streaming_source;

typedef struct janus_streaming_rtp_keyframe {
	gboolean enabled;
	/* If enabled, we store the packets of the last keyframe, to immediately send them for new viewers */
	GList *latest_keyframe;
	/* This is where we store packets while we're still collecting the whole keyframe */
	GList *temp_keyframe;
	guint32 temp_ts;
	janus_mutex mutex;
} janus_streaming_rtp_keyframe;

typedef struct janus_streaming_rtp_relay_packet {
	janus_rtp_header *data;
	gint length;
	gboolean is_rtp;	/* This may be a data packet and not RTP */
	gboolean is_video;
	gboolean is_keyframe;
	gboolean simulcast;
	uint32_t ssrc[3];
	janus_videocodec codec;
	int substream;
	uint32_t timestamp;
	uint16_t seq_number;
	/* The following are only relevant for VP9 SVC*/
	gboolean svc;
	janus_vp9_svc_info svc_info;
	/* The following is only relevant for datachannels */
	gboolean textdata;
} janus_streaming_rtp_relay_packet;
static janus_streaming_rtp_relay_packet exit_packet;
static void janus_streaming_rtp_relay_packet_free(janus_streaming_rtp_relay_packet *pkt) {
	if(pkt == NULL || pkt == &exit_packet)
		return;
	g_free(pkt->data);
	g_free(pkt);

}

#ifdef HAVE_LIBCURL
typedef struct janus_streaming_buffer {
	char *buffer;
	size_t size;
} janus_streaming_buffer;
#endif

typedef struct janus_streaming_rtp_source {
	char *audio_host;
	gint audio_port, remote_audio_port;
	gint audio_rtcp_port, remote_audio_rtcp_port;
	in_addr_t audio_mcast;
	char *video_host;
	gint video_port[3], remote_video_port;
	gint video_rtcp_port, remote_video_rtcp_port;
	in_addr_t video_mcast;
	char *data_host;
	gint data_port;
	janus_recorder *arc;	/* The Janus recorder instance for this streams's audio, if enabled */
	janus_recorder *vrc;	/* The Janus recorder instance for this streams's video, if enabled */
	janus_recorder *drc;	/* The Janus recorder instance for this streams's data, if enabled */
	janus_mutex rec_mutex;	/* Mutex to protect the recorders from race conditions */
	janus_rtp_switching_context context[3];
	int audio_fd;
	int video_fd[3];
	int data_fd;
	int pipefd[2];			/* Just needed to quickly interrupt the poll when it's time to wrap up */
	int audio_rtcp_fd;
	int video_rtcp_fd;
	gboolean simulcast;
	gboolean svc;
	gboolean askew, vskew;
	gint64 last_received_audio;
	gint64 last_received_video;
	gint64 last_received_data;
	uint32_t audio_ssrc;		/* Only needed for fixing outgoing RTCP packets */
	uint32_t video_ssrc;		/* Only needed for fixing outgoing RTCP packets */
	volatile gint need_pli;		/* Whether we need to send a PLI later */
	volatile gint sending_pli;	/* Whether we're currently sending a PLI */
	gint64 pli_latest;			/* Time of latest sent PLI (to avoid flooding) */
	uint32_t lowest_bitrate;	/* Lowest bitrate received by viewers via REMB since last update */
	gint64 remb_latest;			/* Time of latest sent REMB (to avoid flooding) */
	struct sockaddr_storage audio_rtcp_addr, video_rtcp_addr;
#ifdef HAVE_LIBCURL
	gboolean rtsp;
	CURL *curl;
	janus_streaming_buffer *curldata;
	char *rtsp_url;
	char *rtsp_username, *rtsp_password;
	int ka_timeout;
	char *rtsp_ahost, *rtsp_vhost;
	gboolean reconnecting;
	gint64 reconnect_timer;
	janus_mutex rtsp_mutex;
#endif
	janus_streaming_rtp_keyframe keyframe;
	gboolean textdata;
	gboolean buffermsg;
	int rtp_collision;
	void *last_msg;
	janus_mutex buffermsg_mutex;
	janus_network_address audio_iface;
	janus_network_address video_iface;
	janus_network_address data_iface;
	/* Only needed for SRTP support */
	gboolean is_srtp;
	int srtpsuite;
	char *srtpcrypto;
	srtp_t srtp_ctx;
	srtp_policy_t srtp_policy;
	/* If the media is end-to-end encrypted, we may need to know */
	gboolean e2ee;
} janus_streaming_rtp_source;

typedef struct janus_streaming_file_source {
	char *filename;
	gboolean opus;
} janus_streaming_file_source;

/* used for audio/video fd and RTCP fd */
typedef struct multiple_fds {
	int fd;
	int rtcp_fd;
} multiple_fds;

typedef struct janus_streaming_codecs {
	gint audio_pt;
	char *audio_rtpmap;
	char *audio_fmtp;
	janus_videocodec video_codec;
	gint video_pt;
	char *video_rtpmap;
	char *video_fmtp;
} janus_streaming_codecs;

typedef struct janus_streaming_mountpoint {
	guint64 id;			/* Unique mountpoint ID (when using integers) */
	gchar *id_str;		/* Unique mountpoint ID (when using strings) */
	char *name;
	char *description;
	char *metadata;
	gboolean is_private;
	char *secret;
	char *pin;
	gboolean enabled;
	gboolean active;
	GThread *thread;	/* A mountpoint may or may not have a thread */
	janus_streaming_type streaming_type;
	janus_streaming_source streaming_source;
	void *source;	/* Can differ according to the source type */
	GDestroyNotify source_destroy;
	janus_streaming_codecs codecs;
	gboolean audio, video, data;
	GList *viewers;
	int helper_threads;		/* Only relevant for RTP mountpoints */
	GList *threads;			/* Only relevant for RTP mountpoints */
	volatile gint destroyed;
	janus_mutex mutex;
	janus_refcount ref;
} janus_streaming_mountpoint;
GHashTable *mountpoints = NULL, *mountpoints_temp = NULL;
janus_mutex mountpoints_mutex;
static char *admin_key = NULL;

typedef struct janus_streaming_helper {
	janus_streaming_mountpoint *mp;
	guint id;
	GThread *thread;
	int num_viewers;
	GList *viewers;
	GAsyncQueue *queued_packets;
	volatile gint destroyed;
	janus_mutex mutex;
	janus_refcount ref;
} janus_streaming_helper;
static void janus_streaming_helper_destroy(janus_streaming_helper *helper) {
	if(helper && g_atomic_int_compare_and_exchange(&helper->destroyed, 0, 1))
		janus_refcount_decrease(&helper->ref);
}
static void janus_streaming_helper_free(const janus_refcount *helper_ref) {
	janus_streaming_helper *helper = janus_refcount_containerof(helper_ref, janus_streaming_helper, ref);
	/* This helper can be destroyed, free all the resources */
	g_async_queue_unref(helper->queued_packets);
	if(helper->viewers != NULL)
		g_list_free(helper->viewers);
	g_free(helper);
}
static void *janus_streaming_helper_thread(void *data);
static void janus_streaming_helper_rtprtcp_packet(gpointer data, gpointer user_data);

/* Helper to create an RTP live source (e.g., from gstreamer/ffmpeg/vlc/etc.) */
janus_streaming_mountpoint *janus_streaming_create_rtp_source(
		uint64_t id, char *id_str, char *name, char *desc, char *metadata,
		int srtpsuite, char *srtpcrypto, int threads, gboolean e2ee,
		gboolean doaudio, gboolean doaudiortcp, char *amcast, const janus_network_address *aiface,
			uint16_t aport, uint16_t artcpport, uint8_t acodec, char *artpmap, char *afmtp, gboolean doaskew,
		gboolean dovideo, gboolean dovideortcp, char *vmcast, const janus_network_address *viface,
			uint16_t vport, uint16_t vrtcpport, uint8_t vcodec, char *vrtpmap, char *vfmtp, gboolean bufferkf,
			gboolean simulcast, uint16_t vport2, uint16_t vport3, gboolean svc, gboolean dovskew, int rtp_collision,
		gboolean dodata, const janus_network_address *diface, uint16_t dport, gboolean textdata, gboolean buffermsg);
/* Helper to create a file/ondemand live source */
janus_streaming_mountpoint *janus_streaming_create_file_source(
		uint64_t id, char *id_str, char *name, char *desc, char *metadata, char *filename, gboolean live,
		gboolean doaudio, uint8_t acodec, char *artpmap, char *afmtp, gboolean dovideo);
/* Helper to create a rtsp live source */
janus_streaming_mountpoint *janus_streaming_create_rtsp_source(
		uint64_t id, char *id_str, char *name, char *desc, char *metadata,
		char *url, char *username, char *password,
		gboolean doaudio, int audiopt, char *artpmap, char *afmtp,
		gboolean dovideo, int videopt, char *vrtpmap, char *vfmtp, gboolean bufferkf,
		const janus_network_address *iface,
		gboolean error_on_failure);


typedef struct janus_streaming_message {
	janus_plugin_session *handle;
	char *transaction;
	json_t *message;
	json_t *jsep;
} janus_streaming_message;
static GAsyncQueue *messages = NULL;
static janus_streaming_message exit_message;

typedef struct janus_streaming_session {
	janus_plugin_session *handle;
	janus_streaming_mountpoint *mountpoint;
	gint64 sdp_sessid;
	gint64 sdp_version;
	volatile gint started;
	volatile gint paused;
	gboolean audio, video, data;		/* Whether audio, video and/or data must be sent to this listener */
	janus_rtp_switching_context context;
	janus_rtp_simulcasting_context sim_context;
	janus_vp8_simulcast_context vp8_context;
	/* The following are only relevant the mountpoint is VP9-SVC, and are not to be confused with VP8
	 * simulcast, which has similar info (substream/templayer) but in a completely different context */
	int spatial_layer, target_spatial_layer;
	gint64 last_spatial_layer[3];
	int temporal_layer, target_temporal_layer;
	/* If the media is end-to-end encrypted, we may need to know */
	gboolean e2ee;
	janus_mutex mutex;
	volatile gint dataready;
	volatile gint stopping;
	volatile gint renegotiating;
	volatile gint hangingup;
	volatile gint destroyed;
	janus_refcount ref;
} janus_streaming_session;
static GHashTable *sessions;
static janus_mutex sessions_mutex = JANUS_MUTEX_INITIALIZER;

static void janus_streaming_session_destroy(janus_streaming_session *session) {
	if(session && g_atomic_int_compare_and_exchange(&session->destroyed, 0, 1))
		janus_refcount_decrease(&session->ref);
}

static void janus_streaming_session_free(const janus_refcount *session_ref) {
	janus_streaming_session *session = janus_refcount_containerof(session_ref, janus_streaming_session, ref);
	/* Remove the reference to the core plugin session */
	janus_refcount_decrease(&session->handle->ref);
	/* This session can be destroyed, free all the resources */
	g_free(session);
}

static void janus_streaming_mountpoint_destroy(janus_streaming_mountpoint *mountpoint) {
	if(!mountpoint)
		return;
	if(!g_atomic_int_compare_and_exchange(&mountpoint->destroyed, 0, 1))
		return;
	/* If this is an RTP source, interrupt the poll */
	if(mountpoint->streaming_source == janus_streaming_source_rtp) {
		janus_streaming_rtp_source *source = mountpoint->source;
		if(source != NULL && source->pipefd[1] > 0) {
			int code = 1;
			ssize_t res = 0;
			do {
				res = write(source->pipefd[1], &code, sizeof(int));
			} while(res == -1 && errno == EINTR);
		}
	}
	/* Wait for the thread to finish */
	if(mountpoint->thread != NULL)
		g_thread_join(mountpoint->thread);
	/* Get rid of the helper threads, if any */
	if(mountpoint->helper_threads > 0) {
		GList *l = mountpoint->threads;
		while(l) {
			janus_streaming_helper *ht = (janus_streaming_helper *)l->data;
			g_async_queue_push(ht->queued_packets, &exit_packet);
			janus_streaming_helper_destroy(ht);
			l = l->next;
		}
	}
	/* Decrease the counter */
	janus_refcount_decrease(&mountpoint->ref);
}

static void janus_streaming_mountpoint_free(const janus_refcount *mp_ref) {
	janus_streaming_mountpoint *mp = janus_refcount_containerof(mp_ref, janus_streaming_mountpoint, ref);
	/* This mountpoint can be destroyed, free all the resources */

	g_free(mp->id_str);
	g_free(mp->name);
	g_free(mp->description);
	g_free(mp->metadata);
	g_free(mp->secret);
	g_free(mp->pin);
	janus_mutex_lock(&mp->mutex);
	if(mp->viewers != NULL)
		g_list_free(mp->viewers);
	if(mp->threads != NULL) {
		/* Remove the last reference to the helper threads, if any */
		GList *l = mp->threads;
		while(l) {
			janus_streaming_helper *ht = (janus_streaming_helper *)l->data;
			janus_refcount_decrease(&ht->ref);
			l = l->next;
		}
		/* Destroy the list */
		g_list_free(mp->threads);
	}
	janus_mutex_unlock(&mp->mutex);

	if(mp->source != NULL && mp->source_destroy != NULL) {
		mp->source_destroy(mp->source);
	}

	g_free(mp->codecs.audio_rtpmap);
	g_free(mp->codecs.audio_fmtp);
	g_free(mp->codecs.video_rtpmap);
	g_free(mp->codecs.video_fmtp);

	g_free(mp);
}

static void janus_streaming_message_free(janus_streaming_message *msg) {
	if(!msg || msg == &exit_message)
		return;

	if(msg->handle && msg->handle->plugin_handle) {
		janus_streaming_session *session = (janus_streaming_session *)msg->handle->plugin_handle;
		janus_refcount_decrease(&session->ref);
	}
	msg->handle = NULL;

	g_free(msg->transaction);
	msg->transaction = NULL;
	if(msg->message)
		json_decref(msg->message);
	msg->message = NULL;
	if(msg->jsep)
		json_decref(msg->jsep);
	msg->jsep = NULL;

	g_free(msg);
}

#ifdef HAVE_LIBOGG
/* Helper struct to handle the playout of Opus files */
typedef struct janus_streaming_opus_context {
	char *name, *filename;
	FILE *file;
	ogg_sync_state sync;
	ogg_stream_state stream;
	ogg_page page;
	ogg_packet pkt;
	char *oggbuf;
	gint state, headers;
} janus_streaming_opus_context;
/* Helper method to open an Opus file, and make sure it's valid */
static int janus_streaming_opus_context_init(janus_streaming_opus_context *ctx) {
	if(ctx == NULL || ctx->file == NULL)
		return -1;
	fseek(ctx->file, 0, SEEK_SET);
	ogg_stream_clear(&ctx->stream);
	ogg_sync_clear(&ctx->sync);
	if(ogg_sync_init(&ctx->sync) < 0) {
		JANUS_LOG(LOG_ERR, "[%s] Error re-initializing Ogg sync state...\n", ctx->name);
		return -1;
	}
	ctx->headers = 0;
	return 0;
}
/* Helper method to check if an Ogg page begins with an Ogg stream */
static gboolean janus_streaming_ogg_is_opus(ogg_page *page) {
	ogg_stream_state state;
	ogg_packet pkt;
	ogg_stream_init(&state, ogg_page_serialno(page));
	ogg_stream_pagein(&state, page);
	if(ogg_stream_packetout(&state, &pkt) == 1) {
		if(pkt.bytes >= 19 && !memcmp(pkt.packet, "OpusHead", 8)) {
			ogg_stream_clear(&state);
			return 1;
		}
	}
	ogg_stream_clear(&state);
	return FALSE;
}
/* Helper method to traverse the Opus file until we get a packet we can send */
static int janus_streaming_opus_context_read(janus_streaming_opus_context *ctx, char *buffer, int length) {
	if(ctx == NULL || ctx->file == NULL || buffer == NULL)
		return -1;
	/* Check our current state in processing the Ogg file */
	int read = 0;
	if(ctx->state == 0) {
		/* Prepare a buffer, and read from the Ogg file... */
		ctx->oggbuf = ogg_sync_buffer(&ctx->sync, 8192);
		if(ctx->oggbuf == NULL) {
			JANUS_LOG(LOG_ERR, "[%s] ogg_sync_buffer failed...\n", ctx->name);
			return -2;
		}
		read = fread(ctx->oggbuf, 1, 8192, ctx->file);
		if(read == 0 && feof(ctx->file)) {
			/* FIXME We're doing this forever... should this be configurable? */
			JANUS_LOG(LOG_VERB, "[%s] Rewind! (%s)\n", ctx->name, ctx->filename);
			if(janus_streaming_opus_context_init(ctx) < 0)
				return -3;
			return janus_streaming_opus_context_read(ctx, buffer, length);
		}
		if(ogg_sync_wrote(&ctx->sync, read) < 0) {
			JANUS_LOG(LOG_ERR, "[%s] ogg_sync_wrote failed...\n", ctx->name);
			return -4;
		}
		/* Next state: sync pageout */
		ctx->state = 1;
	}
	if(ctx->state == 1) {
		/* Prepare an ogg_page out of the buffer */
		while((read = ogg_sync_pageout(&ctx->sync, &ctx->page)) == 1) {
			/* Let's look for an Opus stream, first of all */
			if(ctx->headers == 0) {
				if(janus_streaming_ogg_is_opus(&ctx->page)) {
					/* This is the start of an Opus stream */
					if(ogg_stream_init(&ctx->stream, ogg_page_serialno(&ctx->page)) < 0) {
						JANUS_LOG(LOG_ERR, "[%s] ogg_stream_init failed...\n", ctx->name);
						return -5;
					}
					ctx->headers++;
				} else if(!ogg_page_bos(&ctx->page)) {
					/* No Opus stream? */
					JANUS_LOG(LOG_ERR, "[%s] No Opus stream...\n", ctx->name);
					return -6;
				} else {
					/* Still waiting for an Opus stream */
					return janus_streaming_opus_context_read(ctx, buffer, length);
				}
			}
			/* Submit the page for packetization */
			if(ogg_stream_pagein(&ctx->stream, &ctx->page) < 0) {
				JANUS_LOG(LOG_ERR, "[%s] ogg_stream_pagein failed...\n", ctx->name);
				return -7;
			}
			/* Time to start reading packets */
			ctx->state = 2;
			break;
		}
		if(read != 1) {
			/* Go back to reading from the file */
			ctx->state = 0;
			return janus_streaming_opus_context_read(ctx, buffer, length);
		}
	}
	if(ctx->state == 2) {
		/* Read and process available packets */
		if(ogg_stream_packetout(&ctx->stream, &ctx->pkt) != 1) {
			/* Go back to reading pages */
			ctx->state = 1;
			return janus_streaming_opus_context_read(ctx, buffer, length);
		} else {
			/* Skip header packets */
			if(ctx->headers == 1 && ctx->pkt.bytes >= 19 && !memcmp(ctx->pkt.packet, "OpusHead", 8)) {
				ctx->headers++;
				return janus_streaming_opus_context_read(ctx, buffer, length);
			}
			if(ctx->headers == 2 && ctx->pkt.bytes >= 16 && !memcmp(ctx->pkt.packet, "OpusTags", 8)) {
				ctx->headers++;
				return janus_streaming_opus_context_read(ctx, buffer, length);
			}
			/* Get the packet duration */
			if(length < ctx->pkt.bytes) {
				JANUS_LOG(LOG_WARN, "[%s] Buffer too short for Opus packet (%d < %ld)\n",
					ctx->name, length, ctx->pkt.bytes);
				return -8;
			}
			memcpy(buffer, ctx->pkt.packet, ctx->pkt.bytes);
			length = ctx->pkt.bytes;
			return length;
		}
	}
	/* If we got here, continue with the iteration */
	return -9;
}
/* Helper method to cleanup an Opus context */
static void janus_streaming_opus_context_cleanup(janus_streaming_opus_context *ctx) {
	if(ctx == NULL)
		return;
	if(ctx->headers > 0)
		ogg_stream_clear(&ctx->stream);
	ogg_sync_clear(&ctx->sync);
}
#endif


/* Helper method to send an RTCP PLI */
static void janus_streaming_rtcp_pli_send(janus_streaming_rtp_source *source) {
	if(source == NULL || source->video_rtcp_fd < 0 || source->video_rtcp_addr.ss_family == 0)
		return;
	if(!g_atomic_int_compare_and_exchange(&source->sending_pli, 0, 1))
		return;
	gint64 now = janus_get_monotonic_time();
	if(now - source->pli_latest < G_USEC_PER_SEC) {
		/* We just sent a PLI less than a second ago, schedule a new delivery later */
		g_atomic_int_set(&source->need_pli, 1);
		g_atomic_int_set(&source->sending_pli, 0);
		return;
	}
	/* Update the time of when we last sent a keyframe request */
	g_atomic_int_set(&source->need_pli, 0);
	source->pli_latest = janus_get_monotonic_time();
	JANUS_LOG(LOG_HUGE, "Sending PLI\n");
	/* Generate a PLI */
	char rtcp_buf[12];
	int rtcp_len = 12;
	janus_rtcp_pli((char *)&rtcp_buf, rtcp_len);
	janus_rtcp_fix_ssrc(NULL, rtcp_buf, rtcp_len, 1, 1, source->video_ssrc);
	/* Send the packet */
	int sent = 0;
	if((sent = sendto(source->video_rtcp_fd, rtcp_buf, rtcp_len, 0,
			(struct sockaddr *)&source->video_rtcp_addr, sizeof(source->video_rtcp_addr))) < 0) {
		JANUS_LOG(LOG_ERR, "Error in sendto... %d (%s)\n", errno, strerror(errno));
	} else {
		JANUS_LOG(LOG_HUGE, "Sent %d/%d bytes\n", sent, rtcp_len);
	}
	g_atomic_int_set(&source->sending_pli, 0);
}

/* Helper method to send an RTCP REMB */
static void janus_streaming_rtcp_remb_send(janus_streaming_rtp_source *source) {
	if(source == NULL || source->video_rtcp_fd < 0 || source->video_rtcp_addr.ss_family == 0)
		return;
	/* Update the time of when we last sent REMB feedback */
	source->remb_latest = janus_get_monotonic_time();
	/* Generate a REMB */
	char rtcp_buf[24];
	int rtcp_len = 24;
	janus_rtcp_remb((char *)(&rtcp_buf), rtcp_len, source->lowest_bitrate);
	janus_rtcp_fix_ssrc(NULL, rtcp_buf, rtcp_len, 1, 1, source->video_ssrc);
	JANUS_LOG(LOG_HUGE, "Sending REMB: %"SCNu32"\n", source->lowest_bitrate);
	/* Reset the lowest bitrate */
	source->lowest_bitrate = 0;
	/* Send the packet */
	int sent = 0;
	if((sent = sendto(source->video_rtcp_fd, rtcp_buf, rtcp_len, 0,
			(struct sockaddr *)&source->video_rtcp_addr, sizeof(source->video_rtcp_addr))) < 0) {
		JANUS_LOG(LOG_ERR, "Error in sendto... %d (%s)\n", errno, strerror(errno));
	} else {
		JANUS_LOG(LOG_HUGE, "Sent %d/%d bytes\n", sent, rtcp_len);
	}
}


/* Error codes */
#define JANUS_STREAMING_ERROR_NO_MESSAGE			450
#define JANUS_STREAMING_ERROR_INVALID_JSON			451
#define JANUS_STREAMING_ERROR_INVALID_REQUEST		452
#define JANUS_STREAMING_ERROR_MISSING_ELEMENT		453
#define JANUS_STREAMING_ERROR_INVALID_ELEMENT		454
#define JANUS_STREAMING_ERROR_NO_SUCH_MOUNTPOINT	455
#define JANUS_STREAMING_ERROR_CANT_CREATE			456
#define JANUS_STREAMING_ERROR_UNAUTHORIZED			457
#define JANUS_STREAMING_ERROR_CANT_SWITCH			458
#define JANUS_STREAMING_ERROR_CANT_RECORD			459
#define JANUS_STREAMING_ERROR_INVALID_STATE			460
#define JANUS_STREAMING_ERROR_UNKNOWN_ERROR			470


/* Plugin implementation */
int janus_streaming_init(janus_callbacks *callback, const char *config_path) {
#ifdef HAVE_LIBCURL
	curl_global_init(CURL_GLOBAL_ALL);
#else
	JANUS_LOG(LOG_WARN, "libcurl not available, Streaming plugin will not have RTSP support\n");
#endif
#ifndef HAVE_LIBOGG
	JANUS_LOG(LOG_WARN, "libogg not available, Streaming plugin will not have file-based Opus streaming\n");
#endif
	if(g_atomic_int_get(&stopping)) {
		/* Still stopping from before */
		return -1;
	}
	if(callback == NULL || config_path == NULL) {
		/* Invalid arguments */
		return -1;
	}

	struct ifaddrs *ifas = NULL;
	if(getifaddrs(&ifas) == -1) {
		JANUS_LOG(LOG_ERR, "Unable to acquire list of network devices/interfaces; some configurations may not work as expected... %d (%s)\n",
			errno, strerror(errno));
	}

	/* Read configuration */
	char filename[255];
	g_snprintf(filename, 255, "%s/%s.jcfg", config_path, JANUS_STREAMING_PACKAGE);
	JANUS_LOG(LOG_VERB, "Configuration file: %s\n", filename);
	config = janus_config_parse(filename);
	if(config == NULL) {
		JANUS_LOG(LOG_WARN, "Couldn't find .jcfg configuration file (%s), trying .cfg\n", JANUS_STREAMING_PACKAGE);
		g_snprintf(filename, 255, "%s/%s.cfg", config_path, JANUS_STREAMING_PACKAGE);
		JANUS_LOG(LOG_VERB, "Configuration file: %s\n", filename);
		config = janus_config_parse(filename);
	}
	config_folder = config_path;
	if(config != NULL)
		janus_config_print(config);

	/* Threads will expect this to be set */
	g_atomic_int_set(&initialized, 1);

	/* Parse configuration to populate the mountpoints */
	if(config != NULL) {
		janus_config_category *config_general = janus_config_get_create(config, NULL, janus_config_type_category, "general");
		/* Any admin key to limit who can "create"? */
		janus_config_item *key = janus_config_get(config, config_general, janus_config_type_item, "admin_key");
		if(key != NULL && key->value != NULL)
			admin_key = g_strdup(key->value);
		janus_config_item *range = janus_config_get(config, config_general, janus_config_type_item, "rtp_port_range");
		if(range && range->value) {
			/* Split in min and max port */
			char *maxport = strrchr(range->value, '-');
			if(maxport != NULL) {
				*maxport = '\0';
				maxport++;
				if(janus_string_to_uint16(range->value, &rtp_range_min) < 0)
					JANUS_LOG(LOG_WARN, "Invalid RTP min port value: %s (assuming 0)\n", range->value);
				if(janus_string_to_uint16(maxport, &rtp_range_max) < 0)
					JANUS_LOG(LOG_WARN, "Invalid RTP max port value: %s (assuming 0)\n", maxport);
				maxport--;
				*maxport = '-';
			}
			if(rtp_range_min > rtp_range_max) {
				uint16_t temp_port = rtp_range_min;
				rtp_range_min = rtp_range_max;
				rtp_range_max = temp_port;
			}
			if(rtp_range_min % 2)
				rtp_range_min++;	/* Pick an even port for RTP */
			if(rtp_range_min > rtp_range_max) {
				JANUS_LOG(LOG_WARN, "Incorrect port range (%u -- %u), switching min and max\n", rtp_range_min, rtp_range_max);
				uint16_t range_temp = rtp_range_max;
				rtp_range_max = rtp_range_min;
				rtp_range_min = range_temp;
			}
			if(rtp_range_max == 0)
				rtp_range_max = 65535;
			rtp_range_slider = rtp_range_min;
			JANUS_LOG(LOG_VERB, "Streaming RTP/RTCP port range: %u -- %u\n", rtp_range_min, rtp_range_max);
		}
		janus_config_item *events = janus_config_get(config, config_general, janus_config_type_item, "events");
		if(events != NULL && events->value != NULL)
			notify_events = janus_is_true(events->value);
		if(!notify_events && callback->events_is_enabled()) {
			JANUS_LOG(LOG_WARN, "Notification of events to handlers disabled for %s\n", JANUS_STREAMING_NAME);
		}
		janus_config_item *ids = janus_config_get(config, config_general, janus_config_type_item, "string_ids");
		if(ids != NULL && ids->value != NULL)
			string_ids = janus_is_true(ids->value);
		if(string_ids) {
			JANUS_LOG(LOG_INFO, "Streaming will use alphanumeric IDs, not numeric\n");
		}
	}
	/* Iterate on all mountpoints */
	mountpoints = g_hash_table_new_full(string_ids ? g_str_hash : g_int64_hash, string_ids ? g_str_equal : g_int64_equal,
		(GDestroyNotify)g_free, (GDestroyNotify)janus_streaming_mountpoint_destroy);
	mountpoints_temp = g_hash_table_new_full(string_ids ? g_str_hash : g_int64_hash, string_ids ? g_str_equal : g_int64_equal,
		(GDestroyNotify)g_free, NULL);
	if(config != NULL) {
		GList *clist = janus_config_get_categories(config, NULL), *cl = clist;
		while(cl != NULL) {
			janus_config_category *cat = (janus_config_category *)cl->data;
			if(cat->name == NULL || !strcasecmp(cat->name, "general")) {
				cl = cl->next;
				continue;
			}
			JANUS_LOG(LOG_VERB, "Adding Streaming mountpoint '%s'\n", cat->name);
			janus_config_item *type = janus_config_get(config, cat, janus_config_type_item, "type");
			if(type == NULL || type->value == NULL) {
				JANUS_LOG(LOG_WARN, "  -- Invalid type, skipping mountpoint '%s'...\n", cat->name);
				cl = cl->next;
				continue;
			}
			janus_config_item *id = janus_config_get(config, cat, janus_config_type_item, "id");
			guint64 mpid = 0;
			if(id == NULL || id->value == NULL) {
				JANUS_LOG(LOG_VERB, "Missing id for mountpoint '%s', will generate a random one...\n", cat->name);
			} else {
				janus_mutex_lock(&mountpoints_mutex);
				if(!string_ids) {
					mpid = g_ascii_strtoull(id->value, 0, 10);
					/* Make sure the ID is completely numeric */
					char mpid_str[30];
					g_snprintf(mpid_str, sizeof(mpid_str), "%"SCNu64, mpid);
					if(strcmp(id->value, mpid_str)) {
						janus_mutex_unlock(&mountpoints_mutex);
						JANUS_LOG(LOG_ERR, "Can't add the Streaming mountpoint '%s', ID '%s' is not numeric...\n",
							cat->name, id->value);
						cl = cl->next;
						continue;
					}
					if(mpid == 0) {
						janus_mutex_unlock(&mountpoints_mutex);
						JANUS_LOG(LOG_ERR, "Can't add the Streaming mountpoint '%s', invalid ID '%s'...\n",
							cat->name, id->value);
						cl = cl->next;
						continue;
					}
				}
				/* Let's make sure the mountpoint doesn't exist already */
				if(g_hash_table_lookup(mountpoints, string_ids ? (gpointer)id->value : (gpointer)&mpid) != NULL) {
					/* It does... */
					janus_mutex_unlock(&mountpoints_mutex);
					JANUS_LOG(LOG_ERR, "Can't add the Streaming mountpoint '%s', ID '%s' already exists...\n",
						cat->name, id->value);
					cl = cl->next;
					continue;
				}
				janus_mutex_unlock(&mountpoints_mutex);
			}
			if(!strcasecmp(type->value, "rtp")) {
				janus_network_address video_iface, audio_iface, data_iface;
				/* RTP live source (e.g., from gstreamer/ffmpeg/vlc/etc.) */
				janus_config_item *desc = janus_config_get(config, cat, janus_config_type_item, "description");
				janus_config_item *md = janus_config_get(config, cat, janus_config_type_item, "metadata");
				janus_config_item *priv = janus_config_get(config, cat, janus_config_type_item, "is_private");
				janus_config_item *secret = janus_config_get(config, cat, janus_config_type_item, "secret");
				janus_config_item *pin = janus_config_get(config, cat, janus_config_type_item, "pin");
				janus_config_item *audio = janus_config_get(config, cat, janus_config_type_item, "audio");
				janus_config_item *askew = janus_config_get(config, cat, janus_config_type_item, "audioskew");
				janus_config_item *video = janus_config_get(config, cat, janus_config_type_item, "video");
				janus_config_item *vskew = janus_config_get(config, cat, janus_config_type_item, "videoskew");
				janus_config_item *vsvc = janus_config_get(config, cat, janus_config_type_item, "videosvc");
				janus_config_item *data = janus_config_get(config, cat, janus_config_type_item, "data");
				janus_config_item *diface = janus_config_get(config, cat, janus_config_type_item, "dataiface");
				janus_config_item *amcast = janus_config_get(config, cat, janus_config_type_item, "audiomcast");
				janus_config_item *aiface = janus_config_get(config, cat, janus_config_type_item, "audioiface");
				janus_config_item *aport = janus_config_get(config, cat, janus_config_type_item, "audioport");
				janus_config_item *artcpport = janus_config_get(config, cat, janus_config_type_item, "audiortcpport");
				janus_config_item *acodec = janus_config_get(config, cat, janus_config_type_item, "audiopt");
				janus_config_item *artpmap = janus_config_get(config, cat, janus_config_type_item, "audiortpmap");
				janus_config_item *afmtp = janus_config_get(config, cat, janus_config_type_item, "audiofmtp");
				janus_config_item *vmcast = janus_config_get(config, cat, janus_config_type_item, "videomcast");
				janus_config_item *viface = janus_config_get(config, cat, janus_config_type_item, "videoiface");
				janus_config_item *vport = janus_config_get(config, cat, janus_config_type_item, "videoport");
				janus_config_item *vrtcpport = janus_config_get(config, cat, janus_config_type_item, "videortcpport");
				janus_config_item *vcodec = janus_config_get(config, cat, janus_config_type_item, "videopt");
				janus_config_item *vrtpmap = janus_config_get(config, cat, janus_config_type_item, "videortpmap");
				janus_config_item *vfmtp = janus_config_get(config, cat, janus_config_type_item, "videofmtp");
				janus_config_item *vkf = janus_config_get(config, cat, janus_config_type_item, "videobufferkf");
				janus_config_item *vsc = janus_config_get(config, cat, janus_config_type_item, "videosimulcast");
				janus_config_item *vport2 = janus_config_get(config, cat, janus_config_type_item, "videoport2");
				janus_config_item *vport3 = janus_config_get(config, cat, janus_config_type_item, "videoport3");
				janus_config_item *dport = janus_config_get(config, cat, janus_config_type_item, "dataport");
				janus_config_item *dbm = janus_config_get(config, cat, janus_config_type_item, "databuffermsg");
				janus_config_item *dt = janus_config_get(config, cat, janus_config_type_item, "datatype");
				janus_config_item *rtpcollision = janus_config_get(config, cat, janus_config_type_item, "collision");
				janus_config_item *threads = janus_config_get(config, cat, janus_config_type_item, "threads");
				janus_config_item *ssuite = janus_config_get(config, cat, janus_config_type_item, "srtpsuite");
				janus_config_item *scrypto = janus_config_get(config, cat, janus_config_type_item, "srtpcrypto");
				janus_config_item *e2ee = janus_config_get(config, cat, janus_config_type_item, "e2ee");
				gboolean is_private = priv && priv->value && janus_is_true(priv->value);
				gboolean doaudio = audio && audio->value && janus_is_true(audio->value);
				gboolean doaskew = audio && askew && askew->value && janus_is_true(askew->value);
				gboolean dovideo = video && video->value && janus_is_true(video->value);
				gboolean dovskew = video && vskew && vskew->value && janus_is_true(vskew->value);
				gboolean dosvc = video && vsvc && vsvc->value && janus_is_true(vsvc->value);
				gboolean dodata = data && data->value && janus_is_true(data->value);
				gboolean bufferkf = video && vkf && vkf->value && janus_is_true(vkf->value);
				gboolean simulcast = video && vsc && vsc->value && janus_is_true(vsc->value);
				if(simulcast && bufferkf) {
					/* FIXME We'll need to take care of this */
					JANUS_LOG(LOG_WARN, "Simulcasting enabled, so disabling buffering of keyframes\n");
					bufferkf = FALSE;
				}
				gboolean buffermsg = data && dbm && dbm->value && janus_is_true(dbm->value);
				gboolean textdata = TRUE;
				if(data && dt && dt->value) {
					if(!strcasecmp(dt->value, "text"))
						textdata = TRUE;
					else if(!strcasecmp(dt->value, "binary"))
						textdata = FALSE;
					else {
						JANUS_LOG(LOG_ERR, "Can't add 'rtp' mountpoint '%s', invalid data type '%s'...\n", cat->name, dt->value);
						cl = cl->next;
						continue;
					}
				}
				if(!doaudio && !dovideo && !dodata) {
					JANUS_LOG(LOG_ERR, "Can't add 'rtp' mountpoint '%s', no audio, video or data have to be streamed...\n", cat->name);
					cl = cl->next;
					continue;
				}
				uint16_t audio_port = 0, audio_rtcp_port = 0;
				if(doaudio &&
						(aport == NULL || aport->value == NULL ||
						janus_string_to_uint16(aport->value, &audio_port) < 0 || audio_port == 0 ||
						acodec == NULL || acodec->value == NULL ||
						artpmap == NULL || artpmap->value == NULL)) {
					JANUS_LOG(LOG_ERR, "Can't add 'rtp' mountpoint '%s', missing mandatory information for audio...\n", cat->name);
					cl = cl->next;
					continue;
				}
				if(doaudio && artcpport != NULL && artcpport->value != NULL &&
						(janus_string_to_uint16(artcpport->value, &audio_rtcp_port) < 0)) {
					JANUS_LOG(LOG_ERR, "Can't add 'rtp' mountpoint '%s', invalid audio RTCP port...\n", cat->name);
					cl = cl->next;
					continue;
				}
				gboolean doaudiortcp = (artcpport != NULL && artcpport->value != NULL);
				if(doaudio && aiface) {
					if(!ifas) {
						JANUS_LOG(LOG_ERR, "Skipping 'rtp' mountpoint '%s', it relies on network configuration but network device information is unavailable...\n", cat->name);
						cl = cl->next;
						continue;
					}
					if(janus_network_lookup_interface(ifas, aiface->value, &audio_iface) != 0) {
						JANUS_LOG(LOG_ERR, "Can't add 'rtp' mountpoint '%s', invalid network interface configuration for audio...\n", cat->name);
						cl = cl->next;
						continue;
					}
				}
				uint16_t video_port = 0, video_port2 = 0, video_port3 = 0, video_rtcp_port = 0;
				if(dovideo &&
						(vport == NULL || vport->value == NULL ||
						janus_string_to_uint16(vport->value, &video_port) < 0 || video_port == 0 ||
						vcodec == NULL || vcodec->value == NULL ||
						vrtpmap == NULL || vrtpmap->value == NULL)) {
					JANUS_LOG(LOG_ERR, "Can't add 'rtp' mountpoint '%s', missing mandatory information for video...\n", cat->name);
					cl = cl->next;
					continue;
				}
				if(dovideo && vrtcpport != NULL && vrtcpport->value != NULL &&
						(janus_string_to_uint16(vrtcpport->value, &video_rtcp_port) < 0)) {
					JANUS_LOG(LOG_ERR, "Can't add 'rtp' mountpoint '%s', invalid video RTCP port...\n", cat->name);
					cl = cl->next;
					continue;
				}
				gboolean dovideortcp = (vrtcpport != NULL && vrtcpport->value != NULL);
				if(dovideo && vport2 != NULL && vport2->value != NULL &&
						(janus_string_to_uint16(vport2->value, &video_port2) < 0)) {
					JANUS_LOG(LOG_ERR, "Can't add 'rtp' mountpoint '%s', invalid simulcast port...\n", cat->name);
					cl = cl->next;
					continue;
				}
				if(dovideo && vport3 != NULL && vport3->value != NULL &&
						(janus_string_to_uint16(vport3->value, &video_port3) < 0)) {
					JANUS_LOG(LOG_ERR, "Can't add 'rtp' mountpoint '%s', invalid simulcast port...\n", cat->name);
					cl = cl->next;
					continue;
				}
				if(dovideo && viface) {
					if(!ifas) {
						JANUS_LOG(LOG_ERR, "Skipping 'rtp' mountpoint '%s', it relies on network configuration but network device information is unavailable...\n", cat->name);
						cl = cl->next;
						continue;
					}
					if(janus_network_lookup_interface(ifas, viface->value, &video_iface) != 0) {
						JANUS_LOG(LOG_ERR, "Can't add 'rtp' mountpoint '%s', invalid network interface configuration for video...\n", cat->name);
						cl = cl->next;
						continue;
					}
				}
				uint16_t data_port = 0;
				if(dodata && (dport == NULL || dport->value == NULL ||
						janus_string_to_uint16(dport->value, &data_port) < 0 || data_port == 0)) {
					JANUS_LOG(LOG_ERR, "Can't add 'rtp' mountpoint '%s', missing mandatory information for data...\n", cat->name);
					cl = cl->next;
					continue;
				}
#ifndef HAVE_SCTP
				if(dodata) {
					JANUS_LOG(LOG_ERR, "Can't add 'rtp' mountpoint '%s': no datachannels support......\n", cat->name);
					cl = cl->next;
					continue;
				}
#endif
				if(dodata && diface) {
					if(!ifas) {
						JANUS_LOG(LOG_ERR, "Skipping 'rtp' mountpoint '%s', it relies on network configuration but network device information is unavailable...\n", cat->name);
						cl = cl->next;
						continue;
					}
					if(janus_network_lookup_interface(ifas, diface->value, &data_iface) != 0) {
						JANUS_LOG(LOG_ERR, "Can't add 'rtp' mountpoint '%s', invalid network interface configuration for data...\n", cat->name);
						cl = cl->next;
						continue;
					}
				}
				if(ssuite && ssuite->value && atoi(ssuite->value) != 32 && atoi(ssuite->value) != 80) {
					JANUS_LOG(LOG_ERR, "Can't add 'rtp' mountpoint '%s', invalid SRTP suite...\n", cat->name);
					cl = cl->next;
					continue;
				}
				if(rtpcollision && rtpcollision->value && atoi(rtpcollision->value) < 0) {
					JANUS_LOG(LOG_ERR, "Can't add 'rtp' mountpoint '%s', invalid collision configuration...\n", cat->name);
					cl = cl->next;
					continue;
				}
				if(threads && threads->value && atoi(threads->value) < 0) {
					JANUS_LOG(LOG_ERR, "Can't add 'rtp' mountpoint '%s', invalid threads configuration...\n", cat->name);
					cl = cl->next;
					continue;
				}
				JANUS_LOG(LOG_VERB, "Audio %s, Video %s, Data %s\n",
					doaudio ? "enabled" : "NOT enabled",
					dovideo ? "enabled" : "NOT enabled",
					dodata ? "enabled" : "NOT enabled");
				janus_streaming_mountpoint *mp = NULL;
				if((mp = janus_streaming_create_rtp_source(
						mpid, (char *)(id ? id->value : NULL),
						(char *)cat->name,
						desc ? (char *)desc->value : NULL,
						md ? (char *)md->value : NULL,
						ssuite && ssuite->value ? atoi(ssuite->value) : 0,
						scrypto && scrypto->value ? (char *)scrypto->value : NULL,
						(threads && threads->value) ? atoi(threads->value) : 0,
						(e2ee && e2ee->value) ? janus_is_true(e2ee->value) : FALSE,
						doaudio, doaudiortcp,
						amcast ? (char *)amcast->value : NULL,
						doaudio && aiface && aiface->value ? &audio_iface : NULL,
						(aport && aport->value) ? audio_port : 0,
						(artcpport && artcpport->value) ? audio_rtcp_port : 0,
						(acodec && acodec->value) ? atoi(acodec->value) : 0,
						artpmap ? (char *)artpmap->value : NULL,
						afmtp ? (char *)afmtp->value : NULL,
						doaskew,
						dovideo, dovideortcp,
						vmcast ? (char *)vmcast->value : NULL,
						dovideo && viface && viface->value ? &video_iface : NULL,
						(vport && vport->value) ? video_port : 0,
						(vrtcpport && vrtcpport->value) ? video_rtcp_port : 0,
						(vcodec && vcodec->value) ? atoi(vcodec->value) : 0,
						vrtpmap ? (char *)vrtpmap->value : NULL,
						vfmtp ? (char *)vfmtp->value : NULL,
						bufferkf,
						simulcast,
						(vport2 && vport2->value) ? video_port2 : 0,
						(vport3 && vport3->value) ? video_port3 : 0,
						dosvc,
						dovskew,
						(rtpcollision && rtpcollision->value) ?  atoi(rtpcollision->value) : 0,
						dodata,
						dodata && diface && diface->value ? &data_iface : NULL,
						(dport && dport->value) ? data_port : 0,
						textdata, buffermsg)) == NULL) {
					JANUS_LOG(LOG_ERR, "Error creating 'rtp' mountpoint '%s'...\n", cat->name);
					cl = cl->next;
					continue;
				}
				mp->is_private = is_private;
				if(secret && secret->value)
					mp->secret = g_strdup(secret->value);
				if(pin && pin->value)
					mp->pin = g_strdup(pin->value);
			} else if(!strcasecmp(type->value, "live")) {
				/* File-based live source */
				janus_config_item *desc = janus_config_get(config, cat, janus_config_type_item, "description");
				janus_config_item *md = janus_config_get(config, cat, janus_config_type_item, "metadata");
				janus_config_item *priv = janus_config_get(config, cat, janus_config_type_item, "is_private");
				janus_config_item *secret = janus_config_get(config, cat, janus_config_type_item, "secret");
				janus_config_item *pin = janus_config_get(config, cat, janus_config_type_item, "pin");
				janus_config_item *file = janus_config_get(config, cat, janus_config_type_item, "filename");
				janus_config_item *audio = janus_config_get(config, cat, janus_config_type_item, "audio");
				janus_config_item *acodec = janus_config_get(config, cat, janus_config_type_item, "audiopt");
				janus_config_item *artpmap = janus_config_get(config, cat, janus_config_type_item, "audiortpmap");
				janus_config_item *afmtp = janus_config_get(config, cat, janus_config_type_item, "audiofmtp");
				janus_config_item *video = janus_config_get(config, cat, janus_config_type_item, "video");
				if(file == NULL || file->value == NULL) {
					JANUS_LOG(LOG_ERR, "Can't add 'live' mountpoint '%s', missing mandatory information...\n", cat->name);
					cl = cl->next;
					continue;
				}
				gboolean is_private = priv && priv->value && janus_is_true(priv->value);
				gboolean doaudio = audio && audio->value && janus_is_true(audio->value);
				gboolean dovideo = video && video->value && janus_is_true(video->value);
				/* We only support audio for file-based streaming at the moment: for streaming
				 * files using other codecs/formats an external tools should feed us RTP instead */
				if(!doaudio || dovideo) {
					JANUS_LOG(LOG_ERR, "Can't add 'live' mountpoint '%s', we only support audio file streaming right now...\n", cat->name);
					cl = cl->next;
					continue;
				}
#ifdef HAVE_LIBOGG
				if(!strstr(file->value, ".opus") && !strstr(file->value, ".alaw") && !strstr(file->value, ".mulaw")) {
					JANUS_LOG(LOG_ERR, "Can't add 'live' mountpoint '%s', unsupported format (we only support Opus and raw mu-Law/a-Law files right now)\n", cat->name);
#else
				if(!strstr(file->value, ".alaw") && !strstr(file->value, ".mulaw")) {
					JANUS_LOG(LOG_ERR, "Can't add 'live' mountpoint '%s', unsupported format (we only support raw mu-Law and a-Law files right now)\n", cat->name);
#endif
					cl = cl->next;
					continue;
				}
				FILE *audiofile = fopen(file->value, "rb");
				if(!audiofile) {
					JANUS_LOG(LOG_ERR, "Can't add 'live' mountpoint, no such file '%s'...\n", file->value);
					cl = cl->next;
					continue;
				}
				fclose(audiofile);

				janus_streaming_mountpoint *mp = NULL;
				if((mp = janus_streaming_create_file_source(
						mpid, (char *)(id ? id->value : NULL),
						(char *)cat->name,
						desc ? (char *)desc->value : NULL,
						md ? (char *)md->value : NULL,
						(char *)file->value, TRUE,
						doaudio,
						(acodec && acodec->value) ? atoi(acodec->value) : 0,
						artpmap ? (char *)artpmap->value : NULL,
						afmtp ? (char *)afmtp->value : NULL,
						dovideo)) == NULL) {
					JANUS_LOG(LOG_ERR, "Error creating 'live' mountpoint '%s'...\n", cat->name);
					cl = cl->next;
					continue;
				}
				mp->is_private = is_private;
				if(secret && secret->value)
					mp->secret = g_strdup(secret->value);
				if(pin && pin->value)
					mp->pin = g_strdup(pin->value);
			} else if(!strcasecmp(type->value, "ondemand")) {
				/* File-based on demand source */
				janus_config_item *desc = janus_config_get(config, cat, janus_config_type_item, "description");
				janus_config_item *md = janus_config_get(config, cat, janus_config_type_item, "metadata");
				janus_config_item *priv = janus_config_get(config, cat, janus_config_type_item, "is_private");
				janus_config_item *secret = janus_config_get(config, cat, janus_config_type_item, "secret");
				janus_config_item *pin = janus_config_get(config, cat, janus_config_type_item, "pin");
				janus_config_item *file = janus_config_get(config, cat, janus_config_type_item, "filename");
				janus_config_item *audio = janus_config_get(config, cat, janus_config_type_item, "audio");
				janus_config_item *acodec = janus_config_get(config, cat, janus_config_type_item, "audiopt");
				janus_config_item *artpmap = janus_config_get(config, cat, janus_config_type_item, "audiortpmap");
				janus_config_item *afmtp = janus_config_get(config, cat, janus_config_type_item, "audiofmtp");
				janus_config_item *video = janus_config_get(config, cat, janus_config_type_item, "video");
				if(file == NULL || file->value == NULL) {
					JANUS_LOG(LOG_ERR, "Can't add 'ondemand' mountpoint '%s', missing mandatory information...\n", cat->name);
					cl = cl->next;
					continue;
				}
				gboolean is_private = priv && priv->value && janus_is_true(priv->value);
				gboolean doaudio = audio && audio->value && janus_is_true(audio->value);
				gboolean dovideo = video && video->value && janus_is_true(video->value);
				/* We only support audio for file-based streaming at the moment: for streaming
				 * files using other codecs/formats an external tools should feed us RTP instead */
				if(!doaudio || dovideo) {
					JANUS_LOG(LOG_ERR, "Can't add 'ondemand' mountpoint '%s', we only support audio file streaming right now...\n", cat->name);
					cl = cl->next;
					continue;
				}
#ifdef HAVE_LIBOGG
				if(!strstr(file->value, ".opus") && !strstr(file->value, ".alaw") && !strstr(file->value, ".mulaw")) {
					JANUS_LOG(LOG_ERR, "Can't add 'live' mountpoint '%s', unsupported format (we only support Opus and raw mu-Law/a-Law files right now)\n", cat->name);
#else
				if(!strstr(file->value, ".alaw") && !strstr(file->value, ".mulaw")) {
					JANUS_LOG(LOG_ERR, "Can't add 'ondemand' mountpoint '%s', unsupported format (we only support raw mu-Law and a-Law files right now)\n", cat->name);
#endif
					cl = cl->next;
					continue;
				}
				FILE *audiofile = fopen(file->value, "rb");
				if(!audiofile) {
					JANUS_LOG(LOG_ERR, "Can't add 'ondemand' mountpoint, no such file '%s'...\n", file->value);
					cl = cl->next;
					continue;
				}
				fclose(audiofile);

				janus_streaming_mountpoint *mp = NULL;
				if((mp = janus_streaming_create_file_source(
						mpid, (char *)(id ? id->value : NULL),
						(char *)cat->name,
						desc ? (char *)desc->value : NULL,
						md ? (char *)md->value : NULL,
						(char *)file->value, FALSE,
						doaudio,
						(acodec && acodec->value) ? atoi(acodec->value) : 0,
						artpmap ? (char *)artpmap->value : NULL,
						afmtp ? (char *)afmtp->value : NULL,
						dovideo)) == NULL) {
					JANUS_LOG(LOG_ERR, "Error creating 'ondemand' mountpoint '%s'...\n", cat->name);
					cl = cl->next;
					continue;
				}
				mp->is_private = is_private;
				if(secret && secret->value)
					mp->secret = g_strdup(secret->value);
				if(pin && pin->value)
					mp->pin = g_strdup(pin->value);
			} else if(!strcasecmp(type->value, "rtsp")) {
#ifndef HAVE_LIBCURL
				JANUS_LOG(LOG_ERR, "Can't add 'rtsp' mountpoint '%s', libcurl support not compiled...\n", cat->name);
				cl = cl->next;
				continue;
#else
				janus_config_item *desc = janus_config_get(config, cat, janus_config_type_item, "description");
				janus_config_item *md = janus_config_get(config, cat, janus_config_type_item, "metadata");
				janus_config_item *priv = janus_config_get(config, cat, janus_config_type_item, "is_private");
				janus_config_item *secret = janus_config_get(config, cat, janus_config_type_item, "secret");
				janus_config_item *pin = janus_config_get(config, cat, janus_config_type_item, "pin");
				janus_config_item *file = janus_config_get(config, cat, janus_config_type_item, "url");
				janus_config_item *username = janus_config_get(config, cat, janus_config_type_item, "rtsp_user");
				janus_config_item *password = janus_config_get(config, cat, janus_config_type_item, "rtsp_pwd");
				janus_config_item *audio = janus_config_get(config, cat, janus_config_type_item, "audio");
				janus_config_item *artpmap = janus_config_get(config, cat, janus_config_type_item, "audiortpmap");
				janus_config_item *acodec = janus_config_get(config, cat, janus_config_type_item, "audiopt");
				janus_config_item *afmtp = janus_config_get(config, cat, janus_config_type_item, "audiofmtp");
				janus_config_item *video = janus_config_get(config, cat, janus_config_type_item, "video");
				janus_config_item *vcodec = janus_config_get(config, cat, janus_config_type_item, "videopt");
				janus_config_item *vrtpmap = janus_config_get(config, cat, janus_config_type_item, "videortpmap");
				janus_config_item *vfmtp = janus_config_get(config, cat, janus_config_type_item, "videofmtp");
				janus_config_item *vkf = janus_config_get(config, cat, janus_config_type_item, "videobufferkf");
				janus_config_item *iface = janus_config_get(config, cat, janus_config_type_item, "rtspiface");
				janus_config_item *failerr = janus_config_get(config, cat, janus_config_type_item, "rtsp_failcheck");
				janus_network_address iface_value;
				if(file == NULL || file->value == NULL) {
					JANUS_LOG(LOG_ERR, "Can't add 'rtsp' mountpoint '%s', missing mandatory information...\n", cat->name);
					cl = cl->next;
					continue;
				}
				gboolean is_private = priv && priv->value && janus_is_true(priv->value);
				gboolean doaudio = audio && audio->value && janus_is_true(audio->value);
				gboolean dovideo = video && video->value && janus_is_true(video->value);
				gboolean bufferkf = video && vkf && vkf->value && janus_is_true(vkf->value);
				gboolean error_on_failure = TRUE;
				if(failerr && failerr->value)
					error_on_failure = janus_is_true(failerr->value);

				if((doaudio || dovideo) && iface && iface->value) {
					if(!ifas) {
						JANUS_LOG(LOG_ERR, "Skipping 'rtsp' mountpoint '%s', it relies on network configuration but network device information is unavailable...\n", cat->name);
						cl = cl->next;
						continue;
					}
					if(janus_network_lookup_interface(ifas, iface->value, &iface_value) != 0) {
						JANUS_LOG(LOG_ERR, "Can't add 'rtsp' mountpoint '%s', invalid network interface configuration for stream...\n", cat->name);
						cl = cl->next;
						continue;
					}
				}

				janus_streaming_mountpoint *mp = NULL;
				if((mp = janus_streaming_create_rtsp_source(
						mpid, (char *)(id ? id->value : NULL),
						(char *)cat->name,
						desc ? (char *)desc->value : NULL,
						md ? (char *)md->value : NULL,
						(char *)file->value,
						username ? (char *)username->value : NULL,
						password ? (char *)password->value : NULL,
						doaudio,
						(acodec && acodec->value) ? atoi(acodec->value) : -1,
						artpmap ? (char *)artpmap->value : NULL,
						afmtp ? (char *)afmtp->value : NULL,
						dovideo,
						(vcodec && vcodec->value) ? atoi(vcodec->value) : -1,
						vrtpmap ? (char *)vrtpmap->value : NULL,
						vfmtp ? (char *)vfmtp->value : NULL,
						bufferkf,
						iface && iface->value ? &iface_value : NULL,
						error_on_failure)) == NULL) {
					JANUS_LOG(LOG_ERR, "Error creating 'rtsp' mountpoint '%s'...\n", cat->name);
					cl = cl->next;
					continue;
				}
				mp->is_private = is_private;
				if(secret && secret->value)
					mp->secret = g_strdup(secret->value);
				if(pin && pin->value)
					mp->pin = g_strdup(pin->value);
#endif
			} else {
				JANUS_LOG(LOG_WARN, "Ignoring unknown mountpoint type '%s' (%s)...\n", type->value, cat->name);
			}
			cl = cl->next;
		}
		g_list_free(clist);
		/* Done: we keep the configuration file open in case we get a "create" or "destroy" with permanent=true */
	}
	if(ifas) {
		freeifaddrs(ifas);
	}

	/* Show available mountpoints */
	janus_mutex_lock(&mountpoints_mutex);
	GHashTableIter iter;
	gpointer value;
	g_hash_table_iter_init(&iter, mountpoints);
	while(g_hash_table_iter_next(&iter, NULL, &value)) {
		janus_streaming_mountpoint *mp = value;
		JANUS_LOG(LOG_VERB, "  ::: [%s][%s] %s (%s, %s, %s, pin: %s)\n", mp->id_str, mp->name, mp->description,
			mp->streaming_type == janus_streaming_type_live ? "live" : "on demand",
			mp->streaming_source == janus_streaming_source_rtp ? "RTP source" : "file source",
			mp->is_private ? "private" : "public",
			mp->pin ? mp->pin : "no pin");
	}
	janus_mutex_unlock(&mountpoints_mutex);

	sessions = g_hash_table_new_full(NULL, NULL, NULL, (GDestroyNotify)janus_streaming_session_destroy);
	messages = g_async_queue_new_full((GDestroyNotify) janus_streaming_message_free);
	/* This is the callback we'll need to invoke to contact the Janus core */
	gateway = callback;

	/* Launch the thread that will handle incoming messages */
	GError *error = NULL;
	handler_thread = g_thread_try_new("streaming handler", janus_streaming_handler, NULL, &error);
	if(error != NULL) {
		g_atomic_int_set(&initialized, 0);
		JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the Streaming handler thread...\n",
			error->code, error->message ? error->message : "??");
		g_error_free(error);
		janus_config_destroy(config);
		return -1;
	}
	JANUS_LOG(LOG_INFO, "%s initialized!\n", JANUS_STREAMING_NAME);
	return 0;
}

void janus_streaming_destroy(void) {
	if(!g_atomic_int_get(&initialized))
		return;
	g_atomic_int_set(&stopping, 1);

	g_async_queue_push(messages, &exit_message);
	if(handler_thread != NULL) {
		g_thread_join(handler_thread);
		handler_thread = NULL;
	}

	/* Remove all mountpoints */
	janus_mutex_lock(&mountpoints_mutex);
	g_hash_table_destroy(mountpoints);
	mountpoints = NULL;
	g_hash_table_destroy(mountpoints_temp);
	mountpoints_temp = NULL;
	janus_mutex_unlock(&mountpoints_mutex);
	janus_mutex_lock(&sessions_mutex);
	g_hash_table_destroy(sessions);
	sessions = NULL;
	janus_mutex_unlock(&sessions_mutex);
	g_async_queue_unref(messages);
	messages = NULL;

	janus_config_destroy(config);
	g_free(admin_key);

	g_atomic_int_set(&initialized, 0);
	g_atomic_int_set(&stopping, 0);
	JANUS_LOG(LOG_INFO, "%s destroyed!\n", JANUS_STREAMING_NAME);
}

int janus_streaming_get_api_compatibility(void) {
	/* Important! This is what your plugin MUST always return: don't lie here or bad things will happen */
	return JANUS_PLUGIN_API_VERSION;
}

int janus_streaming_get_version(void) {
	return JANUS_STREAMING_VERSION;
}

const char *janus_streaming_get_version_string(void) {
	return JANUS_STREAMING_VERSION_STRING;
}

const char *janus_streaming_get_description(void) {
	return JANUS_STREAMING_DESCRIPTION;
}

const char *janus_streaming_get_name(void) {
	return JANUS_STREAMING_NAME;
}

const char *janus_streaming_get_author(void) {
	return JANUS_STREAMING_AUTHOR;
}

const char *janus_streaming_get_package(void) {
	return JANUS_STREAMING_PACKAGE;
}

static janus_streaming_session *janus_streaming_lookup_session(janus_plugin_session *handle) {
	janus_streaming_session *session = NULL;
	if(g_hash_table_contains(sessions, handle)) {
		session = (janus_streaming_session *)handle->plugin_handle;
	}
	return session;
}

void janus_streaming_create_session(janus_plugin_session *handle, int *error) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		*error = -1;
		return;
	}
	janus_streaming_session *session = g_malloc0(sizeof(janus_streaming_session));
	session->handle = handle;
	session->mountpoint = NULL;	/* This will happen later */
	janus_mutex_init(&session->mutex);
	g_atomic_int_set(&session->started, 0);
	g_atomic_int_set(&session->paused, 0);
	g_atomic_int_set(&session->destroyed, 0);
	g_atomic_int_set(&session->hangingup, 0);
	handle->plugin_handle = session;
	janus_refcount_init(&session->ref, janus_streaming_session_free);
	janus_mutex_lock(&sessions_mutex);
	g_hash_table_insert(sessions, handle, session);
	janus_mutex_unlock(&sessions_mutex);

	return;
}

void janus_streaming_destroy_session(janus_plugin_session *handle, int *error) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		*error = -1;
		return;
	}
	janus_mutex_lock(&sessions_mutex);
	janus_streaming_session *session = janus_streaming_lookup_session(handle);
	if(!session) {
		janus_mutex_unlock(&sessions_mutex);
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		*error = -2;
		return;
	}
	JANUS_LOG(LOG_VERB, "Removing streaming session...\n");
	janus_streaming_hangup_media_internal(handle);
	g_hash_table_remove(sessions, handle);
	janus_mutex_unlock(&sessions_mutex);
	return;
}

json_t *janus_streaming_query_session(janus_plugin_session *handle) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		return NULL;
	}
	janus_mutex_lock(&sessions_mutex);
	janus_streaming_session *session = janus_streaming_lookup_session(handle);
	if(!session) {
		janus_mutex_unlock(&sessions_mutex);
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return NULL;
	}
	janus_refcount_increase(&session->ref);
	janus_mutex_unlock(&sessions_mutex);
	/* What is this user watching, if anything? */
	json_t *info = json_object();
	janus_streaming_mountpoint *mp = session->mountpoint;
	json_object_set_new(info, "state", json_string(mp ? "watching" : "idle"));
	if(mp) {
		janus_refcount_increase(&mp->ref);
		json_object_set_new(info, "mountpoint_id", string_ids ? json_string(mp->id_str) : json_integer(mp->id));
		json_object_set_new(info, "mountpoint_name", mp->name ? json_string(mp->name) : NULL);
		janus_mutex_lock(&mp->mutex);
		json_object_set_new(info, "mountpoint_viewers", json_integer(mp->viewers ? g_list_length(mp->viewers) : 0));
		janus_mutex_unlock(&mp->mutex);
		json_t *media = json_object();
		json_object_set_new(media, "audio", session->audio ? json_true() : json_false());
		json_object_set_new(media, "video", session->video ? json_true() : json_false());
		json_object_set_new(media, "data", session->data ? json_true() : json_false());
		json_object_set_new(info, "media", media);
		if(mp->streaming_source == janus_streaming_source_rtp) {
			janus_streaming_rtp_source *source = mp->source;
			if(source->simulcast) {
				json_t *simulcast = json_object();
				json_object_set_new(simulcast, "substream", json_integer(session->sim_context.substream));
				json_object_set_new(simulcast, "substream-target", json_integer(session->sim_context.substream_target));
				json_object_set_new(simulcast, "temporal-layer", json_integer(session->sim_context.templayer));
				json_object_set_new(simulcast, "temporal-layer-target", json_integer(session->sim_context.templayer_target));
				if(session->sim_context.drop_trigger > 0)
					json_object_set_new(simulcast, "fallback", json_integer(session->sim_context.drop_trigger));
				json_object_set_new(info, "simulcast", simulcast);
			}
			if(source->svc) {
				json_t *svc = json_object();
				json_object_set_new(svc, "spatial-layer", json_integer(session->spatial_layer));
				json_object_set_new(svc, "target-spatial-layer", json_integer(session->target_spatial_layer));
				json_object_set_new(svc, "temporal-layer", json_integer(session->temporal_layer));
				json_object_set_new(svc, "target-temporal-layer", json_integer(session->target_temporal_layer));
				json_object_set_new(info, "svc", svc);
			}
		}
		janus_refcount_decrease(&mp->ref);
	}
	if(session->e2ee)
		json_object_set_new(info, "e2ee", json_true());
	json_object_set_new(info, "hangingup", json_integer(g_atomic_int_get(&session->hangingup)));
	json_object_set_new(info, "started", json_integer(g_atomic_int_get(&session->started)));
	json_object_set_new(info, "dataready", json_integer(g_atomic_int_get(&session->dataready)));
	json_object_set_new(info, "paused", json_integer(g_atomic_int_get(&session->paused)));
	json_object_set_new(info, "stopping", json_integer(g_atomic_int_get(&session->stopping)));
	json_object_set_new(info, "destroyed", json_integer(g_atomic_int_get(&session->destroyed)));
	janus_refcount_decrease(&session->ref);
	return info;
}

/* Helper method to process synchronous requests */
static json_t *janus_streaming_process_synchronous_request(janus_streaming_session *session, json_t *message) {
	json_t *request = json_object_get(message, "request");
	const char *request_text = json_string_value(request);

	/* Parse the message */
	int error_code = 0;
	char error_cause[512];
	json_t *root = message;
	json_t *response = NULL;
	struct ifaddrs *ifas = NULL;

	if(!strcasecmp(request_text, "list")) {
		JANUS_LOG(LOG_VERB, "Request for the list of mountpoints\n");
		gboolean lock_mp_list = TRUE;
		if(admin_key != NULL) {
			json_t *admin_key_json = json_object_get(root, "admin_key");
			/* Verify admin_key if it was provided */
			if(admin_key_json != NULL && json_is_string(admin_key_json) && strlen(json_string_value(admin_key_json)) > 0) {
				JANUS_CHECK_SECRET(admin_key, root, "admin_key", error_code, error_cause,
					JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT, JANUS_STREAMING_ERROR_UNAUTHORIZED);
				if(error_code != 0) {
					goto prepare_response;
				} else {
					lock_mp_list = FALSE;
				}
			}
		}
		json_t *list = json_array();
		/* Return a list of all available mountpoints */
		janus_mutex_lock(&mountpoints_mutex);
		GHashTableIter iter;
		gpointer value;
		g_hash_table_iter_init(&iter, mountpoints);
		while(g_hash_table_iter_next(&iter, NULL, &value)) {
			janus_streaming_mountpoint *mp = value;
			if(mp->is_private && lock_mp_list) {
				/* Skip private stream if no valid admin_key was provided */
				JANUS_LOG(LOG_VERB, "Skipping private mountpoint '%s'\n", mp->description);
				continue;
			}
			janus_refcount_increase(&mp->ref);
			json_t *ml = json_object();
			json_object_set_new(ml, "id", string_ids ? json_string(mp->id_str) : json_integer(mp->id));
			json_object_set_new(ml, "type", json_string(mp->streaming_type == janus_streaming_type_live ? "live" : "on demand"));
			json_object_set_new(ml, "description", json_string(mp->description));
			if(mp->metadata) {
				json_object_set_new(ml, "metadata", json_string(mp->metadata));
			}
			json_object_set_new(ml, "enabled", mp->enabled ? json_true() : json_false());
			if(mp->streaming_source == janus_streaming_source_rtp) {
				janus_streaming_rtp_source *source = mp->source;
				gint64 now = janus_get_monotonic_time();
				if(source->audio_fd != -1)
					json_object_set_new(ml, "audio_age_ms", json_integer((now - source->last_received_audio) / 1000));
				if(source->video_fd[0] != -1 || source->video_fd[1] != -1 || source->video_fd[2] != -1)
					json_object_set_new(ml, "video_age_ms", json_integer((now - source->last_received_video) / 1000));
			}
			json_array_append_new(list, ml);
			janus_refcount_decrease(&mp->ref);
		}
		janus_mutex_unlock(&mountpoints_mutex);
		/* Send info back */
		response = json_object();
		json_object_set_new(response, "streaming", json_string("list"));
		json_object_set_new(response, "list", list);
		goto prepare_response;
	} else if(!strcasecmp(request_text, "info")) {
		JANUS_LOG(LOG_VERB, "Request info on a specific mountpoint\n");
		/* Return info on a specific mountpoint */
		if(!string_ids) {
			JANUS_VALIDATE_JSON_OBJECT(root, id_parameters,
				error_code, error_cause, TRUE,
				JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
		} else {
			JANUS_VALIDATE_JSON_OBJECT(root, idstr_parameters,
				error_code, error_cause, TRUE,
				JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
		}
		if(error_code != 0)
			goto prepare_response;
		json_t *id = json_object_get(root, "id");
		guint64 id_value = 0;
		char id_num[30], *id_value_str = NULL;
		if(!string_ids) {
			id_value = json_integer_value(id);
			g_snprintf(id_num, sizeof(id_num), "%"SCNu64, id_value);
			id_value_str = id_num;
		} else {
			id_value_str = (char *)json_string_value(id);
		}
		janus_mutex_lock(&mountpoints_mutex);
		janus_streaming_mountpoint *mp = g_hash_table_lookup(mountpoints,
			string_ids ? (gpointer)id_value_str : (gpointer)&id_value);
		if(mp == NULL) {
			janus_mutex_unlock(&mountpoints_mutex);
			JANUS_LOG(LOG_VERB, "No such mountpoint/stream %s\n", id_value_str);
			error_code = JANUS_STREAMING_ERROR_NO_SUCH_MOUNTPOINT;
			g_snprintf(error_cause, 512, "No such mountpoint/stream %s", id_value_str);
			goto prepare_response;
		}
		janus_refcount_increase(&mp->ref);
		/* Return more info if the right secret is provided */
		gboolean admin = FALSE;
		if(mp->secret) {
			json_t *secret = json_object_get(root, "secret");
			if(secret && json_string_value(secret) && janus_strcmp_const_time(mp->secret, json_string_value(secret)))
				admin = TRUE;
		} else {
			admin = TRUE;
		}
		json_t *ml = json_object();
		json_object_set_new(ml, "id", string_ids ? json_string(mp->id_str) : json_integer(mp->id));
		if(admin && mp->name)
			json_object_set_new(ml, "name", json_string(mp->name));
		if(mp->description)
			json_object_set_new(ml, "description", json_string(mp->description));
		if(mp->metadata)
			json_object_set_new(ml, "metadata", json_string(mp->metadata));
		if(admin && mp->secret)
			json_object_set_new(ml, "secret", json_string(mp->secret));
		if(admin && mp->pin)
			json_object_set_new(ml, "pin", json_string(mp->pin));
		if(admin && mp->is_private)
			json_object_set_new(ml, "is_private", json_true());
		json_object_set_new(ml, "enabled", mp->enabled ? json_true() : json_false());
		if(admin)
			json_object_set_new(ml, "viewers", json_integer(mp->viewers ? g_list_length(mp->viewers) : 0));
		if(mp->audio) {
			json_object_set_new(ml, "audio", json_true());
			if(mp->codecs.audio_pt != -1)
				json_object_set_new(ml, "audiopt", json_integer(mp->codecs.audio_pt));
			if(mp->codecs.audio_rtpmap)
				json_object_set_new(ml, "audiortpmap", json_string(mp->codecs.audio_rtpmap));
			if(mp->codecs.audio_fmtp)
				json_object_set_new(ml, "audiofmtp", json_string(mp->codecs.audio_fmtp));
		}
		if(mp->video) {
			json_object_set_new(ml, "video", json_true());
			if(mp->codecs.video_pt != -1)
				json_object_set_new(ml, "videopt", json_integer(mp->codecs.video_pt));
			if(mp->codecs.video_rtpmap)
				json_object_set_new(ml, "videortpmap", json_string(mp->codecs.video_rtpmap));
			if(mp->codecs.video_fmtp)
				json_object_set_new(ml, "videofmtp", json_string(mp->codecs.video_fmtp));
		}
		if(mp->data) {
			json_object_set_new(ml, "data", json_true());
		}
		json_object_set_new(ml, "type", json_string(mp->streaming_type == janus_streaming_type_live ? "live" : "on demand"));
		if(mp->streaming_source == janus_streaming_source_file) {
			janus_streaming_file_source *source = mp->source;
			if(admin && source->filename)
				json_object_set_new(ml, "filename", json_string(source->filename));
		} else if(mp->streaming_source == janus_streaming_source_rtp) {
			janus_streaming_rtp_source *source = mp->source;
			if(source->is_srtp) {
				json_object_set_new(ml, "srtp", json_true());
			}
			gint64 now = janus_get_monotonic_time();
#ifdef HAVE_LIBCURL
			if(source->rtsp) {
				json_object_set_new(ml, "rtsp", json_true());
				if(admin) {
					if(source->rtsp_url)
						json_object_set_new(ml, "url", json_string(source->rtsp_url));
					if(source->rtsp_username)
						json_object_set_new(ml, "rtsp_user", json_string(source->rtsp_username));
					if(source->rtsp_password)
						json_object_set_new(ml, "rtsp_pwd", json_string(source->rtsp_password));
				}
			}
#endif
			if(source->keyframe.enabled) {
				json_object_set_new(ml, "videobufferkf", json_true());
			}
			if(source->simulcast) {
				json_object_set_new(ml, "videosimulcast", json_true());
			}
			if(source->svc) {
				json_object_set_new(ml, "videosvc", json_true());
			}
			if(source->askew)
				json_object_set_new(ml, "audioskew", json_true());
			if(source->vskew)
				json_object_set_new(ml, "videoskew", json_true());
			if(source->rtp_collision > 0)
				json_object_set_new(ml, "collision", json_integer(source->rtp_collision));
			if(mp->helper_threads > 0)
				json_object_set_new(ml, "threads", json_integer(mp->helper_threads));
			if(admin) {
				if(mp->audio) {
					if(source->audio_host)
						json_object_set_new(ml, "audiohost", json_string(source->audio_host));
					json_object_set_new(ml, "audioport", json_integer(source->audio_port));
					if(source->audio_rtcp_port > -1)
						json_object_set_new(ml, "audiortcpport", json_integer(source->audio_rtcp_port));
				}
				if(mp->video) {
					if(source->video_host)
						json_object_set_new(ml, "videohost", json_string(source->video_host));
					json_object_set_new(ml, "videoport", json_integer(source->video_port[0]));
					if(source->video_rtcp_port > -1)
						json_object_set_new(ml, "videortcpport", json_integer(source->video_rtcp_port));
					if(source->video_port[1] > -1)
						json_object_set_new(ml, "videoport2", json_integer(source->video_port[1]));
					if(source->video_port[2] > -1)
						json_object_set_new(ml, "videoport3", json_integer(source->video_port[2]));
				}
				if(mp->data) {
					if(source->data_host)
						json_object_set_new(ml, "datahost", json_string(source->data_host));
					json_object_set_new(ml, "dataport", json_integer(source->data_port));
				}
			}
			if(source->audio_fd != -1)
				json_object_set_new(ml, "audio_age_ms", json_integer((now - source->last_received_audio) / 1000));
			if(source->video_fd[0] != -1 || source->video_fd[1] != -1 || source->video_fd[2] != -1)
				json_object_set_new(ml, "video_age_ms", json_integer((now - source->last_received_video) / 1000));
			if(source->data_fd != -1)
				json_object_set_new(ml, "data_age_ms", json_integer((now - source->last_received_data) / 1000));
			janus_mutex_lock(&source->rec_mutex);
			if(admin && (source->arc || source->vrc || source->drc)) {
				json_t *recording = json_object();
				if(source->arc && source->arc->filename)
					json_object_set_new(recording, "audio", json_string(source->arc->filename));
				if(source->vrc && source->vrc->filename)
					json_object_set_new(recording, "video", json_string(source->vrc->filename));
				if(source->drc && source->drc->filename)
					json_object_set_new(recording, "data", json_string(source->drc->filename));
				json_object_set_new(ml, "recording", recording);
			}
			janus_mutex_unlock(&source->rec_mutex);
		}
		janus_refcount_decrease(&mp->ref);
		janus_mutex_unlock(&mountpoints_mutex);
		/* Send info back */
		response = json_object();
		json_object_set_new(response, "streaming", json_string("info"));
		json_object_set_new(response, "info", ml);
		goto prepare_response;
	} else if(!strcasecmp(request_text, "create")) {
		/* Create a new stream */
		JANUS_VALIDATE_JSON_OBJECT(root, create_parameters,
			error_code, error_cause, TRUE,
			JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
		if(error_code != 0)
			goto prepare_response;
		if(!string_ids) {
			JANUS_VALIDATE_JSON_OBJECT(root, idopt_parameters,
				error_code, error_cause, TRUE,
				JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
		} else {
			JANUS_VALIDATE_JSON_OBJECT(root, idstropt_parameters,
				error_code, error_cause, TRUE,
				JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
		}
		if(error_code != 0)
			goto prepare_response;
		if(admin_key != NULL) {
			/* An admin key was specified: make sure it was provided, and that it's valid */
			JANUS_VALIDATE_JSON_OBJECT(root, adminkey_parameters,
				error_code, error_cause, TRUE,
				JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
			if(error_code != 0)
				goto prepare_response;
			JANUS_CHECK_SECRET(admin_key, root, "admin_key", error_code, error_cause,
				JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT, JANUS_STREAMING_ERROR_UNAUTHORIZED);
			if(error_code != 0)
				goto prepare_response;
		}

		if(getifaddrs(&ifas) == -1) {
			JANUS_LOG(LOG_ERR, "Unable to acquire list of network devices/interfaces; some configurations may not work as expected... %d (%s)\n",
				errno, strerror(errno));
		}

		json_t *type = json_object_get(root, "type");
		const char *type_text = json_string_value(type);
		json_t *secret = json_object_get(root, "secret");
		json_t *pin = json_object_get(root, "pin");
		json_t *permanent = json_object_get(root, "permanent");
		gboolean save = permanent ? json_is_true(permanent) : FALSE;
		if(save && config == NULL) {
			JANUS_LOG(LOG_ERR, "No configuration file, can't create permanent mountpoint\n");
			error_code = JANUS_STREAMING_ERROR_UNKNOWN_ERROR;
			g_snprintf(error_cause, 512, "No configuration file, can't create permanent mountpoint");
			goto prepare_response;
		}
		json_t *id = json_object_get(root, "id");
		/* Check if an ID has been provided, or if we need to generate one ourselves */
		janus_mutex_lock(&mountpoints_mutex);
		guint64 mpid = string_ids ? 0 : json_integer_value(id);
		char *mpid_str = (char *)(string_ids ? json_string_value(id) : NULL);
		if((!string_ids && mpid > 0) || (string_ids && mpid_str != NULL)) {
			/* Make sure the provided ID isn't already in use */
			if(g_hash_table_lookup(mountpoints, string_ids ? (gpointer)mpid_str : (gpointer)&mpid) != NULL ||
					g_hash_table_lookup(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid) != NULL) {
				janus_mutex_unlock(&mountpoints_mutex);
				JANUS_LOG(LOG_ERR, "A stream with the provided ID already exists\n");
				error_code = JANUS_STREAMING_ERROR_CANT_CREATE;
				g_snprintf(error_cause, 512, "A stream with the provided ID already exists");
				goto prepare_response;
			}
		} else if(!string_ids && mpid == 0) {
			/* Generate a unique numeric ID */
			JANUS_LOG(LOG_VERB, "Missing numeric id, will generate a random one...\n");
			while(mpid == 0) {
				mpid = janus_random_uint64();
				if(g_hash_table_lookup(mountpoints, &mpid) != NULL ||
						g_hash_table_lookup(mountpoints_temp, &mpid) != NULL) {
					/* ID already in use, try another one */
					mpid = 0;
				}
			}
		} else if(string_ids && mpid_str == NULL) {
			/* Generate a unique alphanumeric ID */
			JANUS_LOG(LOG_VERB, "Missing alphanumeric id, will generate a random one...\n");
			while(mpid_str == 0) {
				mpid_str = janus_random_uuid();
				if(g_hash_table_lookup(mountpoints, mpid_str) != NULL ||
						g_hash_table_lookup(mountpoints_temp, mpid_str) != NULL) {
					/* ID already in use, try another one */
					g_free(mpid_str);
					mpid_str = NULL;
				}
			}
		}
		g_hash_table_insert(mountpoints_temp,
			string_ids ? (gpointer)g_strdup(mpid_str) : (gpointer)janus_uint64_dup(mpid),
			GUINT_TO_POINTER(TRUE));
		janus_mutex_unlock(&mountpoints_mutex);
		janus_streaming_mountpoint *mp = NULL;
		if(!strcasecmp(type_text, "rtp")) {
			janus_network_address audio_iface, video_iface, data_iface;
			/* RTP live source (e.g., from gstreamer/ffmpeg/vlc/etc.) */
			JANUS_VALIDATE_JSON_OBJECT(root, rtp_parameters,
				error_code, error_cause, TRUE,
				JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
			if(error_code != 0) {
				janus_mutex_lock(&mountpoints_mutex);
				g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
				janus_mutex_unlock(&mountpoints_mutex);
				goto prepare_response;
			}
			json_t *name = json_object_get(root, "name");
			json_t *desc = json_object_get(root, "description");
			json_t *md = json_object_get(root, "metadata");
			json_t *is_private = json_object_get(root, "is_private");
			json_t *audio = json_object_get(root, "audio");
			json_t *video = json_object_get(root, "video");
			json_t *data = json_object_get(root, "data");
			json_t *rtpcollision = json_object_get(root, "collision");
			json_t *threads = json_object_get(root, "threads");
			json_t *ssuite = json_object_get(root, "srtpsuite");
			json_t *scrypto = json_object_get(root, "srtpcrypto");
			json_t *e2ee = json_object_get(root, "e2ee");
			gboolean doaudio = audio ? json_is_true(audio) : FALSE, doaudiortcp = FALSE;
			gboolean dovideo = video ? json_is_true(video) : FALSE, dovideortcp = FALSE;
			gboolean dodata = data ? json_is_true(data) : FALSE;
			gboolean doaskew = FALSE, dovskew = FALSE, dosvc = FALSE;
			if(!doaudio && !dovideo && !dodata) {
				JANUS_LOG(LOG_ERR, "Can't add 'rtp' stream, no audio, video or data have to be streamed...\n");
				error_code = JANUS_STREAMING_ERROR_CANT_CREATE;
				g_snprintf(error_cause, 512, "Can't add 'rtp' stream, no audio or video have to be streamed...");
				janus_mutex_lock(&mountpoints_mutex);
				g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
				janus_mutex_unlock(&mountpoints_mutex);
				goto prepare_response;
			}
			if(ssuite && json_integer_value(ssuite) != 32 && json_integer_value(ssuite) != 80) {
				JANUS_LOG(LOG_ERR, "Can't add 'rtp' stream, invalid SRTP suite...\n");
				error_code = JANUS_STREAMING_ERROR_CANT_CREATE;
				g_snprintf(error_cause, 512, "Can't add 'rtp' stream, invalid SRTP suite...");
				janus_mutex_lock(&mountpoints_mutex);
				g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
				janus_mutex_unlock(&mountpoints_mutex);
				goto prepare_response;
			}
			uint16_t aport = 0;
			uint16_t artcpport = 0;
			uint8_t acodec = 0;
			char *artpmap = NULL, *afmtp = NULL, *amcast = NULL;
			if(doaudio) {
				JANUS_VALIDATE_JSON_OBJECT(root, rtp_audio_parameters,
					error_code, error_cause, TRUE,
					JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
				if(error_code != 0) {
					janus_mutex_lock(&mountpoints_mutex);
					g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
					janus_mutex_unlock(&mountpoints_mutex);
					goto prepare_response;
				}
				json_t *audiomcast = json_object_get(root, "audiomcast");
				amcast = (char *)json_string_value(audiomcast);
				json_t *audioport = json_object_get(root, "audioport");
				aport = json_integer_value(audioport);
				json_t *audiortcpport = json_object_get(root, "audiortcpport");
				if(audiortcpport) {
					doaudiortcp = TRUE;
					artcpport = json_integer_value(audiortcpport);
				}
				json_t *audiopt = json_object_get(root, "audiopt");
				acodec = json_integer_value(audiopt);
				json_t *audiortpmap = json_object_get(root, "audiortpmap");
				artpmap = (char *)json_string_value(audiortpmap);
				json_t *audiofmtp = json_object_get(root, "audiofmtp");
				afmtp = (char *)json_string_value(audiofmtp);
				json_t *aiface = json_object_get(root, "audioiface");
				if(aiface) {
					const char *miface = (const char *)json_string_value(aiface);
					if(janus_network_lookup_interface(ifas, miface, &audio_iface) != 0) {
						JANUS_LOG(LOG_ERR, "Can't add 'rtp' stream '%s', invalid network interface configuration for audio...\n", (const char *)json_string_value(name));
						error_code = JANUS_STREAMING_ERROR_CANT_CREATE;
						g_snprintf(error_cause, 512, ifas ? "Invalid network interface configuration for audio" : "Unable to query network device information");
						janus_mutex_lock(&mountpoints_mutex);
						g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
						janus_mutex_unlock(&mountpoints_mutex);
						goto prepare_response;
					}
				} else {
					janus_network_address_nullify(&audio_iface);
				}
				json_t *askew = json_object_get(root, "audioskew");
				doaskew = askew ? json_is_true(askew) : FALSE;
			}
			uint16_t vport = 0, vport2 = 0, vport3 = 0;
			uint16_t vrtcpport = 0;
			uint8_t vcodec = 0;
			char *vrtpmap = NULL, *vfmtp = NULL, *vmcast = NULL;
			gboolean bufferkf = FALSE, simulcast = FALSE;
			if(dovideo) {
				JANUS_VALIDATE_JSON_OBJECT(root, rtp_video_parameters,
					error_code, error_cause, TRUE,
					JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
				if(error_code != 0)
					goto prepare_response;
				json_t *videomcast = json_object_get(root, "videomcast");
				vmcast = (char *)json_string_value(videomcast);
				json_t *videoport = json_object_get(root, "videoport");
				vport = json_integer_value(videoport);
				json_t *videortcpport = json_object_get(root, "videortcpport");
				if(videortcpport) {
					dovideortcp = TRUE;
					vrtcpport = json_integer_value(videortcpport);
				}
				json_t *videopt = json_object_get(root, "videopt");
				vcodec = json_integer_value(videopt);
				json_t *videortpmap = json_object_get(root, "videortpmap");
				vrtpmap = (char *)json_string_value(videortpmap);
				json_t *videofmtp = json_object_get(root, "videofmtp");
				vfmtp = (char *)json_string_value(videofmtp);
				json_t *vkf = json_object_get(root, "videobufferkf");
				bufferkf = vkf ? json_is_true(vkf) : FALSE;
				json_t *vsc = json_object_get(root, "videosimulcast");
				simulcast = vsc ? json_is_true(vsc) : FALSE;
				if(simulcast && bufferkf) {
					/* FIXME We'll need to take care of this */
					JANUS_LOG(LOG_WARN, "Simulcasting enabled, so disabling buffering of keyframes\n");
					bufferkf = FALSE;
				}
				json_t *videoport2 = json_object_get(root, "videoport2");
				vport2 = json_integer_value(videoport2);
				json_t *videoport3 = json_object_get(root, "videoport3");
				vport3 = json_integer_value(videoport3);
				json_t *viface = json_object_get(root, "videoiface");
				if(viface) {
					const char *miface = (const char *)json_string_value(viface);
					if(janus_network_lookup_interface(ifas, miface, &video_iface) != 0) {
						JANUS_LOG(LOG_ERR, "Can't add 'rtp' stream '%s', invalid network interface configuration for video...\n", (const char *)json_string_value(name));
						error_code = JANUS_STREAMING_ERROR_CANT_CREATE;
						g_snprintf(error_cause, 512, ifas ? "Invalid network interface configuration for video" : "Unable to query network device information");
						janus_mutex_lock(&mountpoints_mutex);
						g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
						janus_mutex_unlock(&mountpoints_mutex);
						goto prepare_response;
					}
				} else {
					janus_network_address_nullify(&video_iface);
				}
				json_t *vskew = json_object_get(root, "videoskew");
				dovskew = vskew ? json_is_true(vskew) : FALSE;
				json_t *vsvc = json_object_get(root, "videosvc");
				dosvc = vsvc ? json_is_true(vsvc) : FALSE;
			}
			uint16_t dport = 0;
			gboolean textdata = TRUE, buffermsg = FALSE;
			if(dodata) {
				JANUS_VALIDATE_JSON_OBJECT(root, rtp_data_parameters,
					error_code, error_cause, TRUE,
					JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
				if(error_code != 0) {
					janus_mutex_lock(&mountpoints_mutex);
					g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
					janus_mutex_unlock(&mountpoints_mutex);
					goto prepare_response;
				}
#ifdef HAVE_SCTP
				json_t *dataport = json_object_get(root, "dataport");
				dport = json_integer_value(dataport);
				json_t *dbm = json_object_get(root, "databuffermsg");
				buffermsg = dbm ? json_is_true(dbm) : FALSE;
				json_t *dt = json_object_get(root, "datatype");
				if(dt) {
					const char *datatype = (const char *)json_string_value(dt);
					if(!strcasecmp(datatype, "text"))
						textdata = TRUE;
					else if(!strcasecmp(datatype, "binary"))
						textdata = FALSE;
					else {
						JANUS_LOG(LOG_ERR, "Invalid element (datatype can only be text or binary)\n");
						error_code = JANUS_STREAMING_ERROR_INVALID_ELEMENT;
						g_snprintf(error_cause, 512, "Invalid element (datatype can only be text or binary)");
						janus_mutex_lock(&mountpoints_mutex);
						g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
						janus_mutex_unlock(&mountpoints_mutex);
						goto prepare_response;
					}
				}
				json_t *diface = json_object_get(root, "dataiface");
				if(diface) {
					const char *miface = (const char *)json_string_value(diface);
					if(janus_network_lookup_interface(ifas, miface, &data_iface) != 0) {
						JANUS_LOG(LOG_ERR, "Can't add 'rtp' stream '%s', invalid network interface configuration for data...\n", (const char *)json_string_value(name));
						error_code = JANUS_STREAMING_ERROR_CANT_CREATE;
						g_snprintf(error_cause, 512, ifas ? "Invalid network interface configuration for data" : "Unable to query network device information");
						janus_mutex_lock(&mountpoints_mutex);
						g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
						janus_mutex_unlock(&mountpoints_mutex);
						goto prepare_response;
					}
				} else {
					janus_network_address_nullify(&data_iface);
				}
#else
				JANUS_LOG(LOG_ERR, "Can't add 'rtp' stream: no datachannels support...\n");
				error_code = JANUS_STREAMING_ERROR_CANT_CREATE;
				g_snprintf(error_cause, 512, "Can't add 'rtp' stream: no datachannels support...");
				janus_mutex_lock(&mountpoints_mutex);
				g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
				janus_mutex_unlock(&mountpoints_mutex);
				goto prepare_response;
#endif
			}
			JANUS_LOG(LOG_VERB, "Audio %s, Video %s\n", doaudio ? "enabled" : "NOT enabled", dovideo ? "enabled" : "NOT enabled");
			mp = janus_streaming_create_rtp_source(
					mpid, mpid_str,
					name ? (char *)json_string_value(name) : NULL,
					desc ? (char *)json_string_value(desc) : NULL,
					md ? (char *)json_string_value(md) : NULL,
					ssuite ? json_integer_value(ssuite) : 0,
					scrypto ? (char *)json_string_value(scrypto) : NULL,
					threads ? json_integer_value(threads) : 0,
					e2ee ? json_is_true(e2ee) : FALSE,
					doaudio, doaudiortcp, amcast, &audio_iface, aport, artcpport, acodec, artpmap, afmtp, doaskew,
					dovideo, dovideortcp, vmcast, &video_iface, vport, vrtcpport, vcodec, vrtpmap, vfmtp, bufferkf,
					simulcast, vport2, vport3, dosvc, dovskew,
					rtpcollision ? json_integer_value(rtpcollision) : 0,
					dodata, &data_iface, dport, textdata, buffermsg);
			janus_mutex_lock(&mountpoints_mutex);
			g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
			janus_mutex_unlock(&mountpoints_mutex);
			if(mp == NULL) {
				JANUS_LOG(LOG_ERR, "Error creating 'rtp' stream...\n");
				error_code = JANUS_STREAMING_ERROR_CANT_CREATE;
				g_snprintf(error_cause, 512, "Error creating 'rtp' stream");
				goto prepare_response;
			}
			mp->is_private = is_private ? json_is_true(is_private) : FALSE;
		} else if(!strcasecmp(type_text, "live")) {
			/* File-based live source */
			JANUS_VALIDATE_JSON_OBJECT(root, live_parameters,
				error_code, error_cause, TRUE,
				JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
			if(error_code != 0) {
				janus_mutex_lock(&mountpoints_mutex);
				g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
				janus_mutex_unlock(&mountpoints_mutex);
				goto prepare_response;
			}
			json_t *name = json_object_get(root, "name");
			json_t *desc = json_object_get(root, "description");
			json_t *md = json_object_get(root, "metadata");
			json_t *is_private = json_object_get(root, "is_private");
			json_t *file = json_object_get(root, "filename");
			json_t *audio = json_object_get(root, "audio");
			json_t *video = json_object_get(root, "video");
			gboolean doaudio = audio ? json_is_true(audio) : FALSE;
			uint8_t acodec = 0;
			char *artpmap = NULL, *afmtp = NULL;
			if(doaudio) {
				json_t *audiopt = json_object_get(root, "audiopt");
				acodec = json_integer_value(audiopt);
				json_t *audiortpmap = json_object_get(root, "audiortpmap");
				artpmap = (char *)json_string_value(audiortpmap);
				json_t *audiofmtp = json_object_get(root, "audiofmtp");
				afmtp = (char *)json_string_value(audiofmtp);
			}
			gboolean dovideo = video ? json_is_true(video) : FALSE;
			/* We only support audio for file-based streaming at the moment: for streaming
			 * files using other codecs/formats an external tools should feed us RTP instead */
			if(!doaudio || dovideo) {
				JANUS_LOG(LOG_ERR, "Can't add 'live' stream, we only support audio file streaming right now...\n");
				error_code = JANUS_STREAMING_ERROR_CANT_CREATE;
				g_snprintf(error_cause, 512, "Can't add 'live' stream, we only support audio file streaming right now...");
				janus_mutex_lock(&mountpoints_mutex);
				g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
				janus_mutex_unlock(&mountpoints_mutex);
				goto prepare_response;
			}
			char *filename = (char *)json_string_value(file);
#ifdef HAVE_LIBOGG
			if(!strstr(filename, ".opus") && !strstr(filename, ".alaw") && !strstr(filename, ".mulaw")) {
				JANUS_LOG(LOG_ERR, "Can't add 'live' stream, unsupported format (we only support Opus and raw mu-Law/a-Law files right now)\n");
#else
			if(!strstr(filename, ".alaw") && !strstr(filename, ".mulaw")) {
				JANUS_LOG(LOG_ERR, "Can't add 'live' stream, unsupported format (we only support raw mu-Law and a-Law files right now)\n");
#endif
				error_code = JANUS_STREAMING_ERROR_CANT_CREATE;
				g_snprintf(error_cause, 512, "Can't add 'live' stream, unsupported format (we only support raw mu-Law and a-Law files right now)");
				janus_mutex_lock(&mountpoints_mutex);
				g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
				janus_mutex_unlock(&mountpoints_mutex);
				goto prepare_response;
			}
			FILE *audiofile = fopen(filename, "rb");
			if(!audiofile) {
				JANUS_LOG(LOG_ERR, "Can't add 'live' stream, no such file '%s'...\n", filename);
				error_code = JANUS_STREAMING_ERROR_CANT_CREATE;
				g_snprintf(error_cause, 512, "Can't add 'live' stream, no such file '%s'\n", filename);
				janus_mutex_lock(&mountpoints_mutex);
				g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
				janus_mutex_unlock(&mountpoints_mutex);
				goto prepare_response;
			}
			fclose(audiofile);
			mp = janus_streaming_create_file_source(
					mpid, mpid_str,
					name ? (char *)json_string_value(name) : NULL,
					desc ? (char *)json_string_value(desc) : NULL,
					md ? (char *)json_string_value(md) : NULL,
					filename, TRUE,
					doaudio, acodec, artpmap, afmtp, dovideo);
			janus_mutex_lock(&mountpoints_mutex);
			g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
			janus_mutex_unlock(&mountpoints_mutex);
			if(mp == NULL) {
				JANUS_LOG(LOG_ERR, "Error creating 'live' stream...\n");
				error_code = JANUS_STREAMING_ERROR_CANT_CREATE;
				g_snprintf(error_cause, 512, "Error creating 'live' stream");
				goto prepare_response;
			}
			mp->is_private = is_private ? json_is_true(is_private) : FALSE;
		} else if(!strcasecmp(type_text, "ondemand")) {
			/* File-based on demand source */
			JANUS_VALIDATE_JSON_OBJECT(root, ondemand_parameters,
				error_code, error_cause, TRUE,
				JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
			if(error_code != 0) {
				janus_mutex_lock(&mountpoints_mutex);
				g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
				janus_mutex_unlock(&mountpoints_mutex);
				goto prepare_response;
			}
			json_t *name = json_object_get(root, "name");
			json_t *desc = json_object_get(root, "description");
			json_t *md = json_object_get(root, "metadata");
			json_t *is_private = json_object_get(root, "is_private");
			json_t *file = json_object_get(root, "filename");
			json_t *audio = json_object_get(root, "audio");
			json_t *video = json_object_get(root, "video");
			gboolean doaudio = audio ? json_is_true(audio) : FALSE;
			uint8_t acodec = 0;
			char *artpmap = NULL, *afmtp = NULL;
			if(doaudio) {
				json_t *audiopt = json_object_get(root, "audiopt");
				acodec = json_integer_value(audiopt);
				json_t *audiortpmap = json_object_get(root, "audiortpmap");
				artpmap = (char *)json_string_value(audiortpmap);
				json_t *audiofmtp = json_object_get(root, "audiofmtp");
				afmtp = (char *)json_string_value(audiofmtp);
			}
			gboolean dovideo = video ? json_is_true(video) : FALSE;
			/* We only support audio for file-based streaming at the moment: for streaming
			 * files using other codecs/formats an external tools should feed us RTP instead */
			if(!doaudio || dovideo) {
				JANUS_LOG(LOG_ERR, "Can't add 'ondemand' stream, we only support audio file streaming right now...\n");
				error_code = JANUS_STREAMING_ERROR_CANT_CREATE;
				g_snprintf(error_cause, 512, "Can't add 'ondemand' stream, we only support audio file streaming right now...");
				janus_mutex_lock(&mountpoints_mutex);
				g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
				janus_mutex_unlock(&mountpoints_mutex);
				goto prepare_response;
			}
			char *filename = (char *)json_string_value(file);
#ifdef HAVE_LIBOGG
			if(!strstr(filename, ".opus") && !strstr(filename, ".alaw") && !strstr(filename, ".mulaw")) {
				JANUS_LOG(LOG_ERR, "Can't add 'live' stream, unsupported format (we only support Opus and raw mu-Law/a-Law files right now)\n");
#else
			if(!strstr(filename, ".alaw") && !strstr(filename, ".mulaw")) {
				JANUS_LOG(LOG_ERR, "Can't add 'live' stream, unsupported format (we only support raw mu-Law and a-Law files right now)\n");
#endif
				JANUS_LOG(LOG_ERR, "Can't add 'ondemand' stream, unsupported format (we only support raw mu-Law and a-Law files right now)\n");
				error_code = JANUS_STREAMING_ERROR_CANT_CREATE;
				g_snprintf(error_cause, 512, "Can't add 'ondemand' stream, unsupported format (we only support raw mu-Law and a-Law files right now)");
				janus_mutex_lock(&mountpoints_mutex);
				g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
				janus_mutex_unlock(&mountpoints_mutex);
				goto prepare_response;
			}
			FILE *audiofile = fopen(filename, "rb");
			if(!audiofile) {
				JANUS_LOG(LOG_ERR, "Can't add 'ondemand' stream, no such file '%s'...\n", filename);
				error_code = JANUS_STREAMING_ERROR_CANT_CREATE;
				g_snprintf(error_cause, 512, "Can't add 'ondemand' stream, no such file '%s'\n", filename);
				janus_mutex_lock(&mountpoints_mutex);
				g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
				janus_mutex_unlock(&mountpoints_mutex);
				goto prepare_response;
			}
			fclose(audiofile);
			mp = janus_streaming_create_file_source(
					mpid, mpid_str,
					name ? (char *)json_string_value(name) : NULL,
					desc ? (char *)json_string_value(desc) : NULL,
					md ? (char *)json_string_value(md) : NULL,
					filename, FALSE,
					doaudio, acodec, artpmap, afmtp, dovideo);
			janus_mutex_lock(&mountpoints_mutex);
			g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
			janus_mutex_unlock(&mountpoints_mutex);
			if(mp == NULL) {
				JANUS_LOG(LOG_ERR, "Error creating 'ondemand' stream...\n");
				error_code = JANUS_STREAMING_ERROR_CANT_CREATE;
				g_snprintf(error_cause, 512, "Error creating 'ondemand' stream");
				goto prepare_response;
			}
			mp->is_private = is_private ? json_is_true(is_private) : FALSE;
		} else if(!strcasecmp(type_text, "rtsp")) {
#ifndef HAVE_LIBCURL
			JANUS_LOG(LOG_ERR, "Can't create 'rtsp' mountpoint, libcurl support not compiled...\n");
			error_code = JANUS_STREAMING_ERROR_INVALID_ELEMENT;
			g_snprintf(error_cause, 512, "Can't create 'rtsp' mountpoint, libcurl support not compiled...\n");
			goto prepare_response;
#else
			JANUS_VALIDATE_JSON_OBJECT(root, rtsp_parameters,
				error_code, error_cause, TRUE,
				JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
			if(error_code != 0) {
				janus_mutex_lock(&mountpoints_mutex);
				g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
				janus_mutex_unlock(&mountpoints_mutex);
				goto prepare_response;
			}
			/* RTSP source*/
			janus_network_address multicast_iface;
			json_t *name = json_object_get(root, "name");
			json_t *desc = json_object_get(root, "description");
			json_t *md = json_object_get(root, "metadata");
			json_t *is_private = json_object_get(root, "is_private");
			json_t *audio = json_object_get(root, "audio");
			json_t *audiopt = json_object_get(root, "audiopt");
			json_t *audiortpmap = json_object_get(root, "audiortpmap");
			json_t *audiofmtp = json_object_get(root, "audiofmtp");
			json_t *video = json_object_get(root, "video");
			json_t *videopt = json_object_get(root, "videopt");
			json_t *videortpmap = json_object_get(root, "videortpmap");
			json_t *videofmtp = json_object_get(root, "videofmtp");
			json_t *videobufferkf = json_object_get(root, "videobufferkf");
			json_t *url = json_object_get(root, "url");
			json_t *username = json_object_get(root, "rtsp_user");
			json_t *password = json_object_get(root, "rtsp_pwd");
			json_t *iface = json_object_get(root, "rtspiface");
			json_t *failerr = json_object_get(root, "rtsp_failcheck");
			if(failerr == NULL)	/* For an old typo, we support the legacy syntax too */
				failerr = json_object_get(root, "rtsp_check");
			gboolean doaudio = audio ? json_is_true(audio) : FALSE;
			gboolean dovideo = video ? json_is_true(video) : FALSE;
			gboolean error_on_failure = failerr ? json_is_true(failerr) : TRUE;
			if(!doaudio && !dovideo) {
				JANUS_LOG(LOG_ERR, "Can't add 'rtsp' stream, no audio or video have to be streamed...\n");
				error_code = JANUS_STREAMING_ERROR_CANT_CREATE;
				g_snprintf(error_cause, 512, "Can't add 'rtsp' stream, no audio or video have to be streamed...");
				janus_mutex_lock(&mountpoints_mutex);
				g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
				janus_mutex_unlock(&mountpoints_mutex);
				goto prepare_response;
			} else {
				if(iface) {
					const char *miface = (const char *)json_string_value(iface);
					if(janus_network_lookup_interface(ifas, miface, &multicast_iface) != 0) {
						JANUS_LOG(LOG_ERR, "Can't add 'rtsp' stream '%s', invalid network interface configuration for stream...\n", (const char *)json_string_value(name));
						error_code = JANUS_STREAMING_ERROR_CANT_CREATE;
						g_snprintf(error_cause, 512, ifas ? "Invalid network interface configuration for stream" : "Unable to query network device information");
						janus_mutex_lock(&mountpoints_mutex);
						g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
						janus_mutex_unlock(&mountpoints_mutex);
						goto prepare_response;
					}
				} else {
					janus_network_address_nullify(&multicast_iface);
				}
			}
			mp = janus_streaming_create_rtsp_source(
					mpid, mpid_str,
					name ? (char *)json_string_value(name) : NULL,
					desc ? (char *)json_string_value(desc) : NULL,
					md ? (char *)json_string_value(md) : NULL,
					(char *)json_string_value(url),
					username ? (char *)json_string_value(username) : NULL,
					password ? (char *)json_string_value(password) : NULL,
					doaudio, (audiopt ? json_integer_value(audiopt) : -1),
						(char *)json_string_value(audiortpmap), (char *)json_string_value(audiofmtp),
					dovideo, (videopt ? json_integer_value(videopt) : -1),
						(char *)json_string_value(videortpmap), (char *)json_string_value(videofmtp),
						videobufferkf ? json_is_true(videobufferkf) : FALSE,
					&multicast_iface,
					error_on_failure);
			janus_mutex_lock(&mountpoints_mutex);
			g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)mpid_str : (gpointer)&mpid);
			janus_mutex_unlock(&mountpoints_mutex);
			if(mp == NULL) {
				JANUS_LOG(LOG_ERR, "Error creating 'rtsp' stream...\n");
				error_code = JANUS_STREAMING_ERROR_CANT_CREATE;
				g_snprintf(error_cause, 512, "Error creating 'RTSP' stream");
				goto prepare_response;
			}
			mp->is_private = is_private ? json_is_true(is_private) : FALSE;
#endif
		} else {
			JANUS_LOG(LOG_ERR, "Unknown stream type '%s'...\n", type_text);
			error_code = JANUS_STREAMING_ERROR_INVALID_ELEMENT;
			g_snprintf(error_cause, 512, "Unknown stream type '%s'...\n", type_text);
			goto prepare_response;
		}
		/* Any secret? */
		if(secret)
			mp->secret = g_strdup(json_string_value(secret));
		/* Any PIN? */
		if(pin)
			mp->pin = g_strdup(json_string_value(pin));
		if(save) {
			/* This mountpoint is permanent: save to the configuration file too
			 * FIXME: We should check if anything fails... */
			JANUS_LOG(LOG_VERB, "Saving mountpoint %s permanently in config file\n", mp->id_str);
			janus_mutex_lock(&config_mutex);
			char value[BUFSIZ];
			/* The category to add is the mountpoint name */
			janus_config_category *c = janus_config_get_create(config, NULL, janus_config_type_category, mp->name);
			/* Now for the common values */
			janus_config_add(config, c, janus_config_item_create("type", type_text));
			janus_config_add(config, c, janus_config_item_create("id", mp->id_str));
			janus_config_add(config, c, janus_config_item_create("description", mp->description));
			if(mp->metadata)
				janus_config_add(config, c, janus_config_item_create("metadata", mp->metadata));
			if(mp->is_private)
				janus_config_add(config, c, janus_config_item_create("is_private", "yes"));
			/* Per type values */
			if(!strcasecmp(type_text, "rtp")) {
				janus_config_add(config, c, janus_config_item_create("audio", mp->codecs.audio_pt >= 0 ? "yes" : "no"));
				janus_streaming_rtp_source *source = mp->source;
				if(mp->codecs.audio_pt >= 0) {
					g_snprintf(value, BUFSIZ, "%d", source->audio_port);
					janus_config_add(config, c, janus_config_item_create("audioport", value));
					if(source->audio_rtcp_port > 0) {
						g_snprintf(value, BUFSIZ, "%d", source->audio_rtcp_port);
						janus_config_add(config, c, janus_config_item_create("audiortcpport", value));
					}
					json_t *audiomcast = json_object_get(root, "audiomcast");
					if(audiomcast)
						janus_config_add(config, c, janus_config_item_create("audiomcast", json_string_value(audiomcast)));
					g_snprintf(value, BUFSIZ, "%d", mp->codecs.audio_pt);
					janus_config_add(config, c, janus_config_item_create("audiopt", value));
					janus_config_add(config, c, janus_config_item_create("audiortpmap", mp->codecs.audio_rtpmap));
					if(mp->codecs.audio_fmtp)
						janus_config_add(config, c, janus_config_item_create("audiofmtp", mp->codecs.audio_fmtp));
					json_t *aiface = json_object_get(root, "audioiface");
					if(aiface)
						janus_config_add(config, c, janus_config_item_create("audioiface", json_string_value(aiface)));
					if(source->askew)
						janus_config_add(config, c, janus_config_item_create("askew", "yes"));
				}
				janus_config_add(config, c, janus_config_item_create("video", mp->codecs.video_pt > 0 ? "yes" : "no"));
				if(mp->codecs.video_pt > 0) {
					g_snprintf(value, BUFSIZ, "%d", source->video_port[0]);
					janus_config_add(config, c, janus_config_item_create("videoport", value));
					if(source->video_rtcp_port > 0) {
						g_snprintf(value, BUFSIZ, "%d", source->video_rtcp_port);
						janus_config_add(config, c, janus_config_item_create("videortcpport", value));
					}
					json_t *videomcast = json_object_get(root, "videomcast");
					if(videomcast)
						janus_config_add(config, c, janus_config_item_create("videomcast", json_string_value(videomcast)));
					g_snprintf(value, BUFSIZ, "%d", mp->codecs.video_pt);
					janus_config_add(config, c, janus_config_item_create("videopt", value));
					janus_config_add(config, c, janus_config_item_create("videortpmap", mp->codecs.video_rtpmap));
					if(mp->codecs.video_fmtp)
						janus_config_add(config, c, janus_config_item_create("videofmtp", mp->codecs.video_fmtp));
					if(source->keyframe.enabled)
						janus_config_add(config, c, janus_config_item_create("videobufferkf", "yes"));
					if(source->simulcast) {
						janus_config_add(config, c, janus_config_item_create("videosimulcast", "yes"));
						if(source->video_port[1]) {
							g_snprintf(value, BUFSIZ, "%d", source->video_port[1]);
							janus_config_add(config, c, janus_config_item_create("videoport2", value));
						}
						if(source->video_port[2]) {
							g_snprintf(value, BUFSIZ, "%d", source->video_port[2]);
							janus_config_add(config, c, janus_config_item_create("videoport3", value));
						}
					}
					if(source->svc)
						janus_config_add(config, c, janus_config_item_create("videosvc", "yes"));
					json_t *viface = json_object_get(root, "videoiface");
					if(viface)
						janus_config_add(config, c, janus_config_item_create("videoiface", json_string_value(viface)));
					if(source->vskew)
						janus_config_add(config, c, janus_config_item_create("videoskew", "yes"));
				}
				if(source->rtp_collision > 0) {
					g_snprintf(value, BUFSIZ, "%d", source->rtp_collision);
					janus_config_add(config, c, janus_config_item_create("collision", value));
				}
				janus_config_add(config, c, janus_config_item_create("data", mp->data ? "yes" : "no"));
				if(source->data_port > -1) {
					g_snprintf(value, BUFSIZ, "%d", source->data_port);
					janus_config_add(config, c, janus_config_item_create("dataport", value));
					if(source->buffermsg)
						janus_config_add(config, c, janus_config_item_create("databuffermsg", "yes"));
					json_t *diface = json_object_get(root, "dataiface");
					if(diface)
						janus_config_add(config, c, janus_config_item_create("dataiface", json_string_value(diface)));
				}
				if(source->srtpsuite > 0 && source->srtpcrypto) {
					g_snprintf(value, BUFSIZ, "%d", source->srtpsuite);
					janus_config_add(config, c, janus_config_item_create("srtpsuite", value));
					janus_config_add(config, c, janus_config_item_create("srtpcrypto", source->srtpcrypto));
				}
				if(mp->helper_threads > 0) {
					g_snprintf(value, BUFSIZ, "%d", mp->helper_threads);
					janus_config_add(config, c, janus_config_item_create("threads", value));
				}
			} else if(!strcasecmp(type_text, "live") || !strcasecmp(type_text, "ondemand")) {
				janus_streaming_file_source *source = mp->source;
				janus_config_add(config, c, janus_config_item_create("filename", source->filename));
				janus_config_add(config, c, janus_config_item_create("audio", mp->codecs.audio_pt >= 0 ? "yes" : "no"));
				janus_config_add(config, c, janus_config_item_create("video", mp->codecs.video_pt > 0 ? "yes" : "no"));
			} else if(!strcasecmp(type_text, "rtsp")) {
#ifdef HAVE_LIBCURL
				janus_streaming_rtp_source *source = mp->source;
				if(source->rtsp_url)
					janus_config_add(config, c, janus_config_item_create("url", source->rtsp_url));
				if(source->rtsp_username)
					janus_config_add(config, c, janus_config_item_create("rtsp_user", source->rtsp_username));
				if(source->rtsp_password)
					janus_config_add(config, c, janus_config_item_create("rtsp_pwd", source->rtsp_password));
#endif
				janus_config_add(config, c, janus_config_item_create("audio", mp->codecs.audio_pt >= 0 ? "yes" : "no"));
				if(mp->codecs.audio_pt >= 0) {
					if(mp->codecs.audio_rtpmap)
						janus_config_add(config, c, janus_config_item_create("audiortpmap", mp->codecs.audio_rtpmap));
					if(mp->codecs.audio_fmtp)
						janus_config_add(config, c, janus_config_item_create("audiofmtp", mp->codecs.audio_fmtp));
				}
				janus_config_add(config, c, janus_config_item_create("video", mp->codecs.video_pt > 0 ? "yes" : "no"));
				if(mp->codecs.video_pt > 0) {
					if(mp->codecs.video_rtpmap)
						janus_config_add(config, c, janus_config_item_create("videortpmap", mp->codecs.video_rtpmap));
					if(mp->codecs.video_fmtp)
						janus_config_add(config, c, janus_config_item_create("videofmtp", mp->codecs.video_fmtp));
				}
				json_t *iface = json_object_get(root, "rtspiface");
				if(iface)
					janus_config_add(config, c, janus_config_item_create("rtspiface", json_string_value(iface)));
			}
			/* Some more common values */
			if(mp->secret)
				janus_config_add(config, c, janus_config_item_create("secret", mp->secret));
			if(mp->pin)
				janus_config_add(config, c, janus_config_item_create("pin", mp->pin));
			/* Save modified configuration */
			if(janus_config_save(config, config_folder, JANUS_STREAMING_PACKAGE) < 0)
				save = FALSE;	/* This will notify the user the mountpoint is not permanent */
			janus_mutex_unlock(&config_mutex);
		}
		/* Send info back */
		response = json_object();
		json_object_set_new(response, "streaming", json_string("created"));
		json_object_set_new(response, "created", json_string(mp->name));
		json_object_set_new(response, "permanent", save ? json_true() : json_false());
		json_t *ml = json_object();
		json_object_set_new(ml, "id", string_ids ? json_string(mp->id_str) : json_integer(mp->id));
		json_object_set_new(ml, "type", json_string(mp->streaming_type == janus_streaming_type_live ? "live" : "on demand"));
		json_object_set_new(ml, "description", json_string(mp->description));
		json_object_set_new(ml, "is_private", mp->is_private ? json_true() : json_false());
		if(!strcasecmp(type_text, "rtp")) {
			janus_streaming_rtp_source *source = mp->source;
			if(source->audio_fd != -1) {
				if(source->audio_host)
					json_object_set_new(ml, "audio_host", json_string(source->audio_host));
				json_object_set_new(ml, "audio_port", json_integer(source->audio_port));
			}
			if(source->audio_rtcp_fd != -1) {
				json_object_set_new(ml, "audio_rtcp_port", json_integer(source->audio_rtcp_port));
			}
			if(source->video_fd[0] != -1) {
				if(source->video_host)
					json_object_set_new(ml, "video_host", json_string(source->video_host));
				json_object_set_new(ml, "video_port", json_integer(source->video_port[0]));
			}
			if(source->video_rtcp_fd != -1) {
				json_object_set_new(ml, "video_rtcp_port", json_integer(source->video_rtcp_port));
			}
			if(source->video_fd[1] != -1) {
				json_object_set_new(ml, "video_port_2", json_integer(source->video_port[1]));
			}
			if(source->video_fd[2] != -1) {
				json_object_set_new(ml, "video_port_3", json_integer(source->video_port[2]));
			}
			if(source->data_fd != -1) {
				if(source->data_host)
					json_object_set_new(ml, "data_host", json_string(source->data_host));
				json_object_set_new(ml, "data_port", json_integer(source->data_port));
			}
		}
		json_object_set_new(response, "stream", ml);
		/* Also notify event handlers */
		if(notify_events && gateway->events_is_enabled()) {
			json_t *info = json_object();
			json_object_set_new(info, "event", json_string("created"));
			json_object_set_new(info, "id", string_ids ? json_string(mp->id_str) : json_integer(mp->id));
			json_object_set_new(info, "type", json_string(mp->streaming_type == janus_streaming_type_live ? "live" : "on demand"));
			gateway->notify_event(&janus_streaming_plugin, session ? session->handle : NULL, info);
		}
		goto prepare_response;
	} else if(!strcasecmp(request_text, "edit")) {
		JANUS_LOG(LOG_VERB, "Attempt to edit an existing streaming mountpoint\n");
		JANUS_VALIDATE_JSON_OBJECT(root, edit_parameters,
			error_code, error_cause, TRUE,
			JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
		if(error_code != 0)
			goto prepare_response;
		if(!string_ids) {
			JANUS_VALIDATE_JSON_OBJECT(root, id_parameters,
				error_code, error_cause, TRUE,
				JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
		} else {
			JANUS_VALIDATE_JSON_OBJECT(root, idstr_parameters,
				error_code, error_cause, TRUE,
				JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
		}
		if(error_code != 0)
			goto prepare_response;
		/* We only allow for a limited set of properties to be edited */
		json_t *id = json_object_get(root, "id");
		json_t *desc = json_object_get(root, "new_description");
		json_t *md = json_object_get(root, "new_metadata");
		json_t *secret = json_object_get(root, "new_secret");
		json_t *pin = json_object_get(root, "new_pin");
		json_t *is_private = json_object_get(root, "new_is_private");
		json_t *permanent = json_object_get(root, "permanent");
		gboolean save = permanent ? json_is_true(permanent) : FALSE;
		if(save && config == NULL) {
			JANUS_LOG(LOG_ERR, "No configuration file, can't edit mountpoint permanently\n");
			error_code = JANUS_STREAMING_ERROR_UNKNOWN_ERROR;
			g_snprintf(error_cause, 512, "No configuration file, can't edit mountpoint permanently");
			goto prepare_response;
		}
		guint64 id_value = 0;
		char id_num[30], *id_value_str = NULL;
		if(!string_ids) {
			id_value = json_integer_value(id);
			g_snprintf(id_num, sizeof(id_num), "%"SCNu64, id_value);
			id_value_str = id_num;
		} else {
			id_value_str = (char *)json_string_value(id);
		}
		janus_mutex_lock(&mountpoints_mutex);
		janus_streaming_mountpoint *mp = g_hash_table_lookup(mountpoints,
			string_ids ? (gpointer)id_value_str : (gpointer)&id_value);
		if(mp == NULL) {
			janus_mutex_unlock(&mountpoints_mutex);
			JANUS_LOG(LOG_ERR, "No such mountpoint (%s)\n", id_value_str);
			error_code = JANUS_STREAMING_ERROR_NO_SUCH_MOUNTPOINT;
			g_snprintf(error_cause, 512, "No such mountpoint (%s)", id_value_str);
			goto prepare_response;
		}
		janus_refcount_increase(&mp->ref);
		janus_mutex_lock(&mp->mutex);
		/* A secret may be required for this action */
		JANUS_CHECK_SECRET(mp->secret, root, "secret", error_code, error_cause,
			JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT, JANUS_STREAMING_ERROR_UNAUTHORIZED);
		if(error_code != 0) {
			janus_mutex_unlock(&mp->mutex);
			janus_mutex_unlock(&mountpoints_mutex);
			janus_refcount_decrease(&mp->ref);
			goto prepare_response;
		}
		/* Edit the mountpoint properties that were provided */
		if(desc != NULL && strlen(json_string_value(desc)) > 0) {
			char *old_description = mp->description;
			char *new_description = g_strdup(json_string_value(desc));
			mp->description = new_description;
			g_free(old_description);
		}
		if(md != NULL) {
			char *old_metadata = mp->metadata;
			char *new_metadata = g_strdup(json_string_value(md));
			mp->metadata = new_metadata;
			g_free(old_metadata);
		}
		if(is_private)
			mp->is_private = json_is_true(is_private);
		/* A secret may be required for this action */
		JANUS_CHECK_SECRET(mp->secret, root, "secret", error_code, error_cause,
			JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT, JANUS_STREAMING_ERROR_UNAUTHORIZED);
		if(error_code != 0) {
			janus_mutex_unlock(&mp->mutex);
			janus_mutex_unlock(&mountpoints_mutex);
			janus_refcount_decrease(&mp->ref);
			goto prepare_response;
		}
		if(secret && strlen(json_string_value(secret)) > 0) {
			char *old_secret = mp->secret;
			char *new_secret = g_strdup(json_string_value(secret));
			mp->secret = new_secret;
			g_free(old_secret);
		}
		if(pin && strlen(json_string_value(pin)) > 0) {
			char *old_pin = mp->pin;
			char *new_pin = g_strdup(json_string_value(pin));
			mp->pin = new_pin;
			g_free(old_pin);
		}
		if(save) {
			JANUS_LOG(LOG_VERB, "Saving edited mountpoint %s permanently in config file\n", mp->id_str);
			janus_mutex_lock(&config_mutex);
			char value[BUFSIZ];
			/* Remove the old category first */
			janus_config_remove(config, NULL, mp->name);
			/* Now write the room details again */
			janus_config_category *c = janus_config_get_create(config, NULL, janus_config_type_category, mp->name);
			/* Now for the common values at top */
			janus_config_add(config, c, janus_config_item_create("id", mp->id_str));
			janus_config_add(config, c, janus_config_item_create("description", mp->description));
			if(mp->metadata)
				janus_config_add(config, c, janus_config_item_create("metadata", mp->metadata));
			if(mp->is_private)
				janus_config_add(config, c, janus_config_item_create("is_private", "yes"));
			/* Per type values */
			if(mp->streaming_source == janus_streaming_source_rtp) {
				gboolean rtsp = FALSE;
#ifdef HAVE_LIBCURL
				janus_streaming_rtp_source *source = mp->source;
				if(source->rtsp)
						rtsp = TRUE;
#endif
				if(rtsp) {
#ifdef HAVE_LIBCURL
					janus_config_add(config, c, janus_config_item_create("type", "rtsp"));
					if(source->rtsp_url)
						janus_config_add(config, c, janus_config_item_create("url", source->rtsp_url));
					if(source->rtsp_username)
						janus_config_add(config, c, janus_config_item_create("rtsp_user", source->rtsp_username));
					if(source->rtsp_password)
						janus_config_add(config, c, janus_config_item_create("rtsp_pwd", source->rtsp_password));
#endif
					janus_config_add(config, c, janus_config_item_create("audio", mp->codecs.audio_pt >= 0 ? "yes" : "no"));
					if(mp->codecs.audio_pt >= 0) {
						if(mp->codecs.audio_rtpmap)
							janus_config_add(config, c, janus_config_item_create("audiortpmap", mp->codecs.audio_rtpmap));
						if(mp->codecs.audio_fmtp)
							janus_config_add(config, c, janus_config_item_create("audiofmtp", mp->codecs.audio_fmtp));
					}
					janus_config_add(config, c, janus_config_item_create("video", mp->codecs.video_pt > 0 ? "yes" : "no"));
					if(mp->codecs.video_pt > 0) {
						if(mp->codecs.video_rtpmap)
							janus_config_add(config, c, janus_config_item_create("videortpmap", mp->codecs.video_rtpmap));
						if(mp->codecs.video_fmtp)
							janus_config_add(config, c, janus_config_item_create("videofmtp", mp->codecs.video_fmtp));
					}
					json_t *iface = json_object_get(root, "rtspiface");
					if(iface)
						janus_config_add(config, c, janus_config_item_create("rtspiface", json_string_value(iface)));
				} else {
					janus_config_add(config, c, janus_config_item_create("type", "rtp"));
					janus_config_add(config, c, janus_config_item_create("audio", mp->codecs.audio_pt >= 0 ? "yes" : "no"));
					janus_streaming_rtp_source *source = mp->source;
					if(mp->codecs.audio_pt >= 0) {
						g_snprintf(value, BUFSIZ, "%d", source->audio_port);
						janus_config_add(config, c, janus_config_item_create("audioport", value));
						if(source->audio_rtcp_port > 0) {
							g_snprintf(value, BUFSIZ, "%d", source->audio_rtcp_port);
							janus_config_add(config, c, janus_config_item_create("audiortcpport", value));
						}
						json_t *audiomcast = json_object_get(root, "audiomcast");
						if(audiomcast)
							janus_config_add(config, c, janus_config_item_create("audiomcast", json_string_value(audiomcast)));
						g_snprintf(value, BUFSIZ, "%d", mp->codecs.audio_pt);
						janus_config_add(config, c, janus_config_item_create("audiopt", value));
						janus_config_add(config, c, janus_config_item_create("audiortpmap", mp->codecs.audio_rtpmap));
						if(mp->codecs.audio_fmtp)
							janus_config_add(config, c, janus_config_item_create("audiofmtp", mp->codecs.audio_fmtp));
						json_t *aiface = json_object_get(root, "audioiface");
						if(aiface)
							janus_config_add(config, c, janus_config_item_create("audioiface", json_string_value(aiface)));
						if(source->askew)
							janus_config_add(config, c, janus_config_item_create("askew", "yes"));
					}
					janus_config_add(config, c, janus_config_item_create("video", mp->codecs.video_pt > 0 ? "yes" : "no"));
					if(mp->codecs.video_pt > 0) {
						g_snprintf(value, BUFSIZ, "%d", source->video_port[0]);
						janus_config_add(config, c, janus_config_item_create("videoport", value));
						if(source->video_rtcp_port > 0) {
							g_snprintf(value, BUFSIZ, "%d", source->video_rtcp_port);
							janus_config_add(config, c, janus_config_item_create("videortcpport", value));
						}
						json_t *videomcast = json_object_get(root, "videomcast");
						if(videomcast)
							janus_config_add(config, c, janus_config_item_create("videomcast", json_string_value(videomcast)));
						g_snprintf(value, BUFSIZ, "%d", mp->codecs.video_pt);
						janus_config_add(config, c, janus_config_item_create("videopt", value));
						janus_config_add(config, c, janus_config_item_create("videortpmap", mp->codecs.video_rtpmap));
						if(mp->codecs.video_fmtp)
							janus_config_add(config, c, janus_config_item_create("videofmtp", mp->codecs.video_fmtp));
						if(source->keyframe.enabled)
							janus_config_add(config, c, janus_config_item_create("videobufferkf", "yes"));
						if(source->simulcast) {
							janus_config_add(config, c, janus_config_item_create("videosimulcast", "yes"));
							if(source->video_port[1]) {
								g_snprintf(value, BUFSIZ, "%d", source->video_port[1]);
								janus_config_add(config, c, janus_config_item_create("videoport2", value));
							}
							if(source->video_port[2]) {
								g_snprintf(value, BUFSIZ, "%d", source->video_port[2]);
								janus_config_add(config, c, janus_config_item_create("videoport3", value));
							}
						}
						if(source->svc)
							janus_config_add(config, c, janus_config_item_create("videosvc", "yes"));
						json_t *viface = json_object_get(root, "videoiface");
						if(viface)
							janus_config_add(config, c, janus_config_item_create("videoiface", json_string_value(viface)));
						if(source->vskew)
							janus_config_add(config, c, janus_config_item_create("videoskew", "yes"));
					}
					if(source->rtp_collision > 0) {
						g_snprintf(value, BUFSIZ, "%d", source->rtp_collision);
						janus_config_add(config, c, janus_config_item_create("collision", value));
					}
					janus_config_add(config, c, janus_config_item_create("data", mp->data ? "yes" : "no"));
					if(source->data_port > -1) {
						g_snprintf(value, BUFSIZ, "%d", source->data_port);
						janus_config_add(config, c, janus_config_item_create("dataport", value));
						if(source->buffermsg)
							janus_config_add(config, c, janus_config_item_create("databuffermsg", "yes"));
						json_t *diface = json_object_get(root, "dataiface");
						if(diface)
							janus_config_add(config, c, janus_config_item_create("dataiface", json_string_value(diface)));
					}
					if(source->srtpsuite > 0 && source->srtpcrypto) {
						g_snprintf(value, BUFSIZ, "%d", source->srtpsuite);
						janus_config_add(config, c, janus_config_item_create("srtpsuite", value));
						janus_config_add(config, c, janus_config_item_create("srtpcrypto", source->srtpcrypto));
					}
					if(mp->helper_threads > 0) {
						g_snprintf(value, BUFSIZ, "%d", mp->helper_threads);
						janus_config_add(config, c, janus_config_item_create("threads", value));
					}
				}
			} else {
				janus_config_add(config, c, janus_config_item_create("type", (mp->streaming_type == janus_streaming_type_live) ? "live" : "ondemand"));
				janus_streaming_file_source *source = mp->source;
				janus_config_add(config, c, janus_config_item_create("filename", source->filename));
				janus_config_add(config, c, janus_config_item_create("audio", mp->codecs.audio_pt >= 0 ? "yes" : "no"));
				janus_config_add(config, c, janus_config_item_create("video", mp->codecs.video_pt > 0 ? "yes" : "no"));
			}
			/* Some more common values */
			if(mp->secret)
				janus_config_add(config, c, janus_config_item_create("secret", mp->secret));
			if(mp->pin)
				janus_config_add(config, c, janus_config_item_create("pin", mp->pin));
			/* Save modified configuration */
			if(janus_config_save(config, config_folder, JANUS_STREAMING_PACKAGE) < 0)
				save = FALSE;	/* This will notify the user the mountpoint is not permanent */
			janus_mutex_unlock(&config_mutex);
		}
		/* Prepare response/notification */
		response = json_object();
		json_object_set_new(response, "streaming", json_string("edited"));
		json_object_set_new(response, "id", string_ids ? json_string(mp->id_str) : json_integer(mp->id));
		json_object_set_new(response, "permanent", save ? json_true() : json_false());
		/* Also notify event handlers */
		if(notify_events && gateway->events_is_enabled()) {
			json_t *info = json_object();
			json_object_set_new(info, "event", json_string("edited"));
			json_object_set_new(info, "id", string_ids ? json_string(mp->id_str) : json_integer(mp->id));
			gateway->notify_event(&janus_streaming_plugin, session ? session->handle : NULL, info);
		}
		janus_mutex_unlock(&mp->mutex);
		janus_mutex_unlock(&mountpoints_mutex);
		janus_refcount_decrease(&mp->ref);
		/* Done */
		JANUS_LOG(LOG_VERB, "Streaming mountpoint edited\n");
		goto prepare_response;
	} else if(!strcasecmp(request_text, "destroy")) {
		/* Get rid of an existing stream (notice this doesn't remove it from the config file, though) */
		JANUS_VALIDATE_JSON_OBJECT(root, destroy_parameters,
			error_code, error_cause, TRUE,
			JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
		if(error_code != 0)
			goto prepare_response;
		if(!string_ids) {
			JANUS_VALIDATE_JSON_OBJECT(root, id_parameters,
				error_code, error_cause, TRUE,
				JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
		} else {
			JANUS_VALIDATE_JSON_OBJECT(root, idstr_parameters,
				error_code, error_cause, TRUE,
				JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
		}
		if(error_code != 0)
			goto prepare_response;
		json_t *id = json_object_get(root, "id");
		guint64 id_value = 0;
		char id_num[30], *id_value_str = NULL;
		if(!string_ids) {
			id_value = json_integer_value(id);
			g_snprintf(id_num, sizeof(id_num), "%"SCNu64, id_value);
			id_value_str = id_num;
		} else {
			id_value_str = (char *)json_string_value(id);
		}
		json_t *permanent = json_object_get(root, "permanent");
		gboolean save = permanent ? json_is_true(permanent) : FALSE;
		if(save && config == NULL) {
			JANUS_LOG(LOG_ERR, "No configuration file, can't destroy mountpoint permanently\n");
			error_code = JANUS_STREAMING_ERROR_UNKNOWN_ERROR;
			g_snprintf(error_cause, 512, "No configuration file, can't destroy mountpoint permanently");
			goto prepare_response;
		}
		janus_mutex_lock(&mountpoints_mutex);
		janus_streaming_mountpoint *mp = g_hash_table_lookup(mountpoints,
			string_ids ? (gpointer)id_value_str : (gpointer)&id_value);
		if(mp == NULL) {
			janus_mutex_unlock(&mountpoints_mutex);
			JANUS_LOG(LOG_VERB, "No such mountpoint/stream %s\n", id_value_str);
			error_code = JANUS_STREAMING_ERROR_NO_SUCH_MOUNTPOINT;
			g_snprintf(error_cause, 512, "No such mountpoint/stream %s", id_value_str);
			goto prepare_response;
		}
		janus_refcount_increase(&mp->ref);
		/* A secret may be required for this action */
		JANUS_CHECK_SECRET(mp->secret, root, "secret", error_code, error_cause,
			JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT, JANUS_STREAMING_ERROR_UNAUTHORIZED);
		if(error_code != 0) {
			janus_refcount_decrease(&mp->ref);
			janus_mutex_unlock(&mountpoints_mutex);
			goto prepare_response;
		}
		JANUS_LOG(LOG_VERB, "Request to unmount mountpoint/stream %s\n", id_value_str);
		/* Remove mountpoint from the hashtable: this will get it destroyed eventually */
		g_hash_table_remove(mountpoints,
			string_ids ? (gpointer)id_value_str : (gpointer)&id_value);
		/* FIXME Should we kick the current viewers as well? */
		janus_mutex_lock(&mp->mutex);
		GList *viewer = g_list_first(mp->viewers);
		/* Prepare JSON event */
		json_t *event = json_object();
		json_object_set_new(event, "streaming", json_string("event"));
		json_t *result = json_object();
		json_object_set_new(result, "status", json_string("stopped"));
		json_object_set_new(event, "result", result);
		while(viewer) {
			janus_streaming_session *s = (janus_streaming_session *)viewer->data;
			if(s == NULL) {
				mp->viewers = g_list_remove_all(mp->viewers, s);
				viewer = g_list_first(mp->viewers);
				continue;
			}
			janus_mutex_lock(&session->mutex);
			if(s->mountpoint != mp) {
				mp->viewers = g_list_remove_all(mp->viewers, s);
				viewer = g_list_first(mp->viewers);
				janus_mutex_unlock(&session->mutex);
				continue;
			}
			g_atomic_int_set(&s->stopping, 1);
			g_atomic_int_set(&s->started, 0);
			g_atomic_int_set(&s->paused, 0);
			s->mountpoint = NULL;
			/* Tell the core to tear down the PeerConnection, hangup_media will do the rest */
			gateway->push_event(s->handle, &janus_streaming_plugin, NULL, event, NULL);
			gateway->close_pc(s->handle);
			janus_refcount_decrease(&s->ref);
			janus_refcount_decrease(&mp->ref);
			if(mp->streaming_source == janus_streaming_source_rtp) {
				/* Remove the viewer from the helper threads too, if any */
				if(mp->helper_threads > 0) {
					GList *l = mp->threads;
					while(l) {
						janus_streaming_helper *ht = (janus_streaming_helper *)l->data;
						janus_mutex_lock(&ht->mutex);
						if(g_list_find(ht->viewers, s) != NULL) {
							ht->num_viewers--;
							ht->viewers = g_list_remove_all(ht->viewers, s);
							janus_mutex_unlock(&ht->mutex);
							JANUS_LOG(LOG_VERB, "Removing viewer from helper thread #%d (destroy)\n", ht->id);
							break;
						}
						janus_mutex_unlock(&ht->mutex);
						l = l->next;
					}
				}
			}
			mp->viewers = g_list_remove_all(mp->viewers, s);
			viewer = g_list_first(mp->viewers);
			janus_mutex_unlock(&session->mutex);
		}
		json_decref(event);
		janus_mutex_unlock(&mp->mutex);
		if(save) {
			/* This change is permanent: save to the configuration file too
			 * FIXME: We should check if anything fails... */
			JANUS_LOG(LOG_VERB, "Destroying mountpoint %s (%s) permanently in config file\n", mp->id_str, mp->name);
			janus_mutex_lock(&config_mutex);
			/* The category to remove is the mountpoint name */
			janus_config_remove(config, NULL, mp->name);
			/* Save modified configuration */
			if(janus_config_save(config, config_folder, JANUS_STREAMING_PACKAGE) < 0)
				save = FALSE;	/* This will notify the user the mountpoint is not permanent */
			janus_mutex_unlock(&config_mutex);
		}
		janus_refcount_decrease(&mp->ref);
		/* Also notify event handlers */
		if(notify_events && gateway->events_is_enabled()) {
			json_t *info = json_object();
			json_object_set_new(info, "event", json_string("destroyed"));
			json_object_set_new(info, "id", string_ids ? json_string(id_value_str) : json_integer(id_value));
			gateway->notify_event(&janus_streaming_plugin, session ? session->handle : NULL, info);
		}
		janus_mutex_unlock(&mountpoints_mutex);
		/* Send info back */
		response = json_object();
		json_object_set_new(response, "streaming", json_string("destroyed"));
		json_object_set_new(response, "destroyed", string_ids ? json_string(id_value_str) : json_integer(id_value));
		goto prepare_response;
	} else if(!strcasecmp(request_text, "recording")) {
		/* We can start/stop recording a live, RTP-based stream */
		JANUS_VALIDATE_JSON_OBJECT(root, recording_parameters,
			error_code, error_cause, TRUE,
			JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
		if(error_code != 0)
			goto prepare_response;
		json_t *action = json_object_get(root, "action");
		const char *action_text = json_string_value(action);
		if(strcasecmp(action_text, "start") && strcasecmp(action_text, "stop")) {
			JANUS_LOG(LOG_ERR, "Invalid action (should be start|stop)\n");
			error_code = JANUS_STREAMING_ERROR_INVALID_ELEMENT;
			g_snprintf(error_cause, 512, "Invalid action (should be start|stop)");
			goto prepare_response;
		}
		if(!string_ids) {
			JANUS_VALIDATE_JSON_OBJECT(root, id_parameters,
				error_code, error_cause, TRUE,
				JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
		} else {
			JANUS_VALIDATE_JSON_OBJECT(root, idstr_parameters,
				error_code, error_cause, TRUE,
				JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
		}
		if(error_code != 0)
			goto prepare_response;
		json_t *id = json_object_get(root, "id");
		guint64 id_value = 0;
		char id_num[30], *id_value_str = NULL;
		if(!string_ids) {
			id_value = json_integer_value(id);
			g_snprintf(id_num, sizeof(id_num), "%"SCNu64, id_value);
			id_value_str = id_num;
		} else {
			id_value_str = (char *)json_string_value(id);
		}
		janus_mutex_lock(&mountpoints_mutex);
		janus_streaming_mountpoint *mp = g_hash_table_lookup(mountpoints,
			string_ids ? (gpointer)id_value_str : (gpointer)&id_value);
		if(mp == NULL) {
			janus_mutex_unlock(&mountpoints_mutex);
			JANUS_LOG(LOG_VERB, "No such mountpoint/stream %s\n", id_value_str);
			error_code = JANUS_STREAMING_ERROR_NO_SUCH_MOUNTPOINT;
			g_snprintf(error_cause, 512, "No such mountpoint/stream %s", id_value_str);
			goto prepare_response;
		}
		janus_refcount_increase(&mp->ref);
		if(mp->streaming_type != janus_streaming_type_live || mp->streaming_source != janus_streaming_source_rtp) {
			janus_refcount_decrease(&mp->ref);
			janus_mutex_unlock(&mountpoints_mutex);
			JANUS_LOG(LOG_ERR, "Recording is only available on RTP-based live streams\n");
			error_code = JANUS_STREAMING_ERROR_INVALID_REQUEST;
			g_snprintf(error_cause, 512, "Recording is only available on RTP-based live streams");
			goto prepare_response;
		}
		/* A secret may be required for this action */
		JANUS_CHECK_SECRET(mp->secret, root, "secret", error_code, error_cause,
			JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT, JANUS_STREAMING_ERROR_UNAUTHORIZED);
		if(error_code != 0) {
			janus_refcount_decrease(&mp->ref);
			janus_mutex_unlock(&mountpoints_mutex);
			goto prepare_response;
		}
		janus_streaming_rtp_source *source = mp->source;
		if(!strcasecmp(action_text, "start")) {
			/* Start a recording for audio and/or video */
			JANUS_VALIDATE_JSON_OBJECT(root, recording_start_parameters,
				error_code, error_cause, TRUE,
				JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
			if(error_code != 0) {
				janus_refcount_decrease(&mp->ref);
				janus_mutex_unlock(&mountpoints_mutex);
				goto prepare_response;
			}
			json_t *audio = json_object_get(root, "audio");
			json_t *video = json_object_get(root, "video");
			json_t *data = json_object_get(root, "data");
			janus_recorder *arc = NULL, *vrc = NULL, *drc = NULL;
			if((audio && source->arc) || (video && source->vrc) || (data && source->drc)) {
				janus_refcount_decrease(&mp->ref);
				janus_mutex_unlock(&mountpoints_mutex);
				JANUS_LOG(LOG_ERR, "Recording for audio, video and/or data already started for this stream\n");
				error_code = JANUS_STREAMING_ERROR_INVALID_REQUEST;
				g_snprintf(error_cause, 512, "Recording for audio, video and/or data already started for this stream");
				goto prepare_response;
			}
			if(!audio && !video && !data) {
				janus_refcount_decrease(&mp->ref);
				janus_mutex_unlock(&mountpoints_mutex);
				JANUS_LOG(LOG_ERR, "Missing audio, video and/or data\n");
				error_code = JANUS_STREAMING_ERROR_INVALID_REQUEST;
				g_snprintf(error_cause, 512, "Missing audio, video and/or data");
				goto prepare_response;
			}
			if(audio) {
				const char *codec = NULL;
				if (!mp->codecs.audio_rtpmap)
					JANUS_LOG(LOG_ERR, "[%s] Audio RTP map is uninitialized\n", mp->name);
				else if(strstr(mp->codecs.audio_rtpmap, "opus") || strstr(mp->codecs.audio_rtpmap, "OPUS"))
					codec = "opus";
				else if(strstr(mp->codecs.audio_rtpmap, "pcma") || strstr(mp->codecs.audio_rtpmap, "PCMA"))
					codec = "pcma";
				else if(strstr(mp->codecs.audio_rtpmap, "pcmu") || strstr(mp->codecs.audio_rtpmap, "PCMU"))
					codec = "pcmu";
				else if(strstr(mp->codecs.audio_rtpmap, "g722") || strstr(mp->codecs.audio_rtpmap, "G722"))
					codec = "g722";
				const char *audiofile = json_string_value(audio);
				arc = janus_recorder_create(NULL, codec, (char *)audiofile);
				if(arc == NULL) {
					JANUS_LOG(LOG_ERR, "[%s] Error starting recorder for audio\n", mp->name);
					janus_refcount_decrease(&mp->ref);
					janus_mutex_unlock(&mountpoints_mutex);
					error_code = JANUS_STREAMING_ERROR_CANT_RECORD;
					g_snprintf(error_cause, 512, "Error starting recorder for audio");
					goto prepare_response;
				}
				/* If media is encrypted, mark it in the recording */
				if(source->e2ee)
					janus_recorder_encrypted(arc);
				JANUS_LOG(LOG_INFO, "[%s] Audio recording started\n", mp->name);
			}
			if(video) {
				const char *codec = NULL;
				if (!mp->codecs.video_rtpmap)
					JANUS_LOG(LOG_ERR, "[%s] Video RTP map is uninitialized\n", mp->name);
				else if(strstr(mp->codecs.video_rtpmap, "vp8") || strstr(mp->codecs.video_rtpmap, "VP8"))
					codec = "vp8";
				else if(strstr(mp->codecs.video_rtpmap, "vp9") || strstr(mp->codecs.video_rtpmap, "VP9"))
					codec = "vp9";
				else if(strstr(mp->codecs.video_rtpmap, "h264") || strstr(mp->codecs.video_rtpmap, "H264"))
					codec = "h264";
				else if(strstr(mp->codecs.video_rtpmap, "av1") || strstr(mp->codecs.video_rtpmap, "AV1"))
					codec = "av1";
				else if(strstr(mp->codecs.video_rtpmap, "h265") || strstr(mp->codecs.video_rtpmap, "H265"))
					codec = "h265";
				const char *videofile = json_string_value(video);
				vrc = janus_recorder_create(NULL, codec, (char *)videofile);
				if(vrc == NULL) {
					if(arc != NULL) {
						janus_recorder_close(arc);
						janus_recorder_destroy(arc);
						arc = NULL;
					}
					JANUS_LOG(LOG_ERR, "[%s] Error starting recorder for video\n", mp->name);
					janus_refcount_decrease(&mp->ref);
					janus_mutex_unlock(&mountpoints_mutex);
					error_code = JANUS_STREAMING_ERROR_CANT_RECORD;
					g_snprintf(error_cause, 512, "Error starting recorder for video");
					goto prepare_response;
				}
				/* If media is encrypted, mark it in the recording */
				if(source->e2ee)
					janus_recorder_encrypted(vrc);
				JANUS_LOG(LOG_INFO, "[%s] Video recording started\n", mp->name);
			}
			if(data) {
				const char *datafile = json_string_value(data);
				drc = janus_recorder_create(NULL, "text", (char *)datafile);
				if(drc == NULL) {
					if(arc != NULL) {
						janus_recorder_close(arc);
						janus_recorder_destroy(arc);
						arc = NULL;
					}
					if(vrc != NULL) {
						janus_recorder_close(vrc);
						janus_recorder_destroy(vrc);
						vrc = NULL;
					}
					JANUS_LOG(LOG_ERR, "[%s] Error starting recorder for data\n", mp->name);
					janus_refcount_decrease(&mp->ref);
					janus_mutex_unlock(&mountpoints_mutex);
					error_code = JANUS_STREAMING_ERROR_CANT_RECORD;
					g_snprintf(error_cause, 512, "Error starting recorder for data");
					goto prepare_response;
				}
				JANUS_LOG(LOG_INFO, "[%s] Data recording started\n", mp->name);
			}
			if(arc != NULL)
				source->arc = arc;
			if(vrc != NULL)
				source->vrc = vrc;
			if(drc != NULL)
				source->drc = drc;
			janus_refcount_decrease(&mp->ref);
			janus_mutex_unlock(&mountpoints_mutex);
			/* Send a success response back */
			response = json_object();
			json_object_set_new(response, "streaming", json_string("ok"));
			goto prepare_response;
		} else if(!strcasecmp(action_text, "stop")) {
			/* Stop the recording */
			JANUS_VALIDATE_JSON_OBJECT(root, recording_stop_parameters,
				error_code, error_cause, TRUE,
				JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
			if(error_code != 0) {
				janus_mutex_unlock(&mountpoints_mutex);
				goto prepare_response;
			}
			json_t *audio = json_object_get(root, "audio");
			json_t *video = json_object_get(root, "video");
			json_t *data = json_object_get(root, "data");
			if(!audio && !video) {
				janus_mutex_unlock(&mountpoints_mutex);
				JANUS_LOG(LOG_ERR, "Missing audio and/or video\n");
				error_code = JANUS_STREAMING_ERROR_INVALID_REQUEST;
				g_snprintf(error_cause, 512, "Missing audio and/or video");
				goto prepare_response;
			}
			janus_mutex_lock(&source->rec_mutex);
			if(audio && json_is_true(audio) && source->arc) {
				/* Close the audio recording */
				janus_recorder_close(source->arc);
				JANUS_LOG(LOG_INFO, "[%s] Closed audio recording %s\n", mp->name, source->arc->filename ? source->arc->filename : "??");
				janus_recorder *tmp = source->arc;
				source->arc = NULL;
				janus_recorder_destroy(tmp);
			}
			if(video && json_is_true(video) && source->vrc) {
				/* Close the video recording */
				janus_recorder_close(source->vrc);
				JANUS_LOG(LOG_INFO, "[%s] Closed video recording %s\n", mp->name, source->vrc->filename ? source->vrc->filename : "??");
				janus_recorder *tmp = source->vrc;
				source->vrc = NULL;
				janus_recorder_destroy(tmp);
			}
			if(data && json_is_true(data) && source->drc) {
				/* Close the data recording */
				janus_recorder_close(source->drc);
				JANUS_LOG(LOG_INFO, "[%s] Closed data recording %s\n", mp->name, source->drc->filename ? source->drc->filename : "??");
				janus_recorder *tmp = source->drc;
				source->drc = NULL;
				janus_recorder_destroy(tmp);
			}
			janus_mutex_unlock(&source->rec_mutex);
			janus_refcount_decrease(&mp->ref);
			janus_mutex_unlock(&mountpoints_mutex);
			/* Send a success response back */
			response = json_object();
			json_object_set_new(response, "streaming", json_string("ok"));
			goto prepare_response;
		}
	} else if(!strcasecmp(request_text, "enable") || !strcasecmp(request_text, "disable")) {
		/* A request to enable/disable a mountpoint */
		if(!string_ids) {
			JANUS_VALIDATE_JSON_OBJECT(root, id_parameters,
				error_code, error_cause, TRUE,
				JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
		} else {
			JANUS_VALIDATE_JSON_OBJECT(root, idstr_parameters,
				error_code, error_cause, TRUE,
				JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
		}
		if(error_code != 0)
			goto prepare_response;
		json_t *id = json_object_get(root, "id");
		guint64 id_value = 0;
		char id_num[30], *id_value_str = NULL;
		if(!string_ids) {
			id_value = json_integer_value(id);
			g_snprintf(id_num, sizeof(id_num), "%"SCNu64, id_value);
			id_value_str = id_num;
		} else {
			id_value_str = (char *)json_string_value(id);
		}
		janus_mutex_lock(&mountpoints_mutex);
		janus_streaming_mountpoint *mp = g_hash_table_lookup(mountpoints,
			string_ids ? (gpointer)id_value_str : (gpointer)&id_value);
		if(mp == NULL) {
			janus_mutex_unlock(&mountpoints_mutex);
			JANUS_LOG(LOG_VERB, "No such mountpoint/stream %s\n", id_value_str);
			error_code = JANUS_STREAMING_ERROR_NO_SUCH_MOUNTPOINT;
			g_snprintf(error_cause, 512, "No such mountpoint/stream %s", id_value_str);
			goto prepare_response;
		}
		janus_refcount_increase(&mp->ref);
		/* A secret may be required for this action */
		JANUS_CHECK_SECRET(mp->secret, root, "secret", error_code, error_cause,
			JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT, JANUS_STREAMING_ERROR_UNAUTHORIZED);
		if(error_code != 0) {
			janus_refcount_decrease(&mp->ref);
			janus_mutex_unlock(&mountpoints_mutex);
			goto prepare_response;
		}
		if(!strcasecmp(request_text, "enable")) {
			/* Enable a previously disabled mountpoint */
			JANUS_LOG(LOG_INFO, "[%s] Stream enabled\n", mp->name);
			mp->enabled = TRUE;
			/* FIXME: Should we notify the viewers, or is this up to the controller application? */
		} else {
			/* Disable a previously enabled mountpoint */
			JANUS_VALIDATE_JSON_OBJECT(root, disable_parameters,
				error_code, error_cause, TRUE,
				JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
			if(error_code != 0)
				goto prepare_response;
			mp->enabled = FALSE;
			gboolean stop_recording = TRUE;
			json_t *stop_rec = json_object_get(root, "stop_recording");
			if (stop_rec) {
				stop_recording = json_is_true(stop_rec);
			}
			JANUS_LOG(LOG_INFO, "[%s] Stream disabled (stop_recording=%s)\n", mp->name, stop_recording ? "yes" : "no");
			/* Any recording to close? */
			if(mp->streaming_source == janus_streaming_source_rtp && stop_recording) {
				janus_streaming_rtp_source *source = mp->source;
				janus_mutex_lock(&source->rec_mutex);
				if(source->arc) {
					janus_recorder_close(source->arc);
					JANUS_LOG(LOG_INFO, "[%s] Closed audio recording %s\n", mp->name, source->arc->filename ? source->arc->filename : "??");
					janus_recorder *tmp = source->arc;
					source->arc = NULL;
					janus_recorder_destroy(tmp);
				}
				if(source->vrc) {
					janus_recorder_close(source->vrc);
					JANUS_LOG(LOG_INFO, "[%s] Closed video recording %s\n", mp->name, source->vrc->filename ? source->vrc->filename : "??");
					janus_recorder *tmp = source->vrc;
					source->vrc = NULL;
					janus_recorder_destroy(tmp);
				}
				if(source->drc) {
					janus_recorder_close(source->drc);
					JANUS_LOG(LOG_INFO, "[%s] Closed data recording %s\n", mp->name, source->drc->filename ? source->drc->filename : "??");
					janus_recorder *tmp = source->drc;
					source->drc = NULL;
					janus_recorder_destroy(tmp);
				}
				janus_mutex_unlock(&source->rec_mutex);
			}
			/* FIXME: Should we notify the viewers, or is this up to the controller application? */
		}
		janus_refcount_decrease(&mp->ref);
		janus_mutex_unlock(&mountpoints_mutex);
		/* Send a success response back */
		response = json_object();
		json_object_set_new(response, "streaming", json_string("ok"));
		goto prepare_response;
	} else {
		/* Not a request we recognize, don't do anything */
		return NULL;
	}

prepare_response:
		{
			if(ifas) {
				freeifaddrs(ifas);
			}

			if(error_code == 0 && !response) {
				error_code = JANUS_STREAMING_ERROR_UNKNOWN_ERROR;
				g_snprintf(error_cause, 512, "Invalid response");
			}
			if(error_code != 0) {
				/* Prepare JSON error event */
				response = json_object();
				json_object_set_new(response, "streaming", json_string("event"));
				json_object_set_new(response, "error_code", json_integer(error_code));
				json_object_set_new(response, "error", json_string(error_cause));
			}
			return response;
		}

}

struct janus_plugin_result *janus_streaming_handle_message(janus_plugin_session *handle, char *transaction, json_t *message, json_t *jsep) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return janus_plugin_result_new(JANUS_PLUGIN_ERROR, g_atomic_int_get(&stopping) ? "Shutting down" : "Plugin not initialized", NULL);

	/* Pre-parse the message */
	int error_code = 0;
	char error_cause[512];
	json_t *root = message;
	json_t *response = NULL;

	janus_mutex_lock(&sessions_mutex);
	janus_streaming_session *session = janus_streaming_lookup_session(handle);
	if(!session) {
		janus_mutex_unlock(&sessions_mutex);
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		error_code = JANUS_STREAMING_ERROR_UNKNOWN_ERROR;
		g_snprintf(error_cause, 512, "%s", "No session associated with this handle...");
		goto plugin_response;
	}
	/* Increase the reference counter for this session: we'll decrease it after we handle the message */
	janus_refcount_increase(&session->ref);
	janus_mutex_unlock(&sessions_mutex);
	if(g_atomic_int_get(&session->destroyed)) {
		JANUS_LOG(LOG_ERR, "Session has already been destroyed...\n");
		error_code = JANUS_STREAMING_ERROR_UNKNOWN_ERROR;
		g_snprintf(error_cause, 512, "%s", "Session has already been destroyed...");
		goto plugin_response;
	}

	if(message == NULL) {
		JANUS_LOG(LOG_ERR, "No message??\n");
		error_code = JANUS_STREAMING_ERROR_NO_MESSAGE;
		g_snprintf(error_cause, 512, "%s", "No message??");
		goto plugin_response;
	}
	if(!json_is_object(root)) {
		JANUS_LOG(LOG_ERR, "JSON error: not an object\n");
		error_code = JANUS_STREAMING_ERROR_INVALID_JSON;
		g_snprintf(error_cause, 512, "JSON error: not an object");
		goto plugin_response;
	}
	/* Get the request first */
	JANUS_VALIDATE_JSON_OBJECT(root, request_parameters,
		error_code, error_cause, TRUE,
		JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
	if(error_code != 0)
		goto plugin_response;
	json_t *request = json_object_get(root, "request");
	/* Some requests ('create' and 'destroy') can be handled synchronously */
	const char *request_text = json_string_value(request);
	/* We have a separate method to process synchronous requests, as those may
	 * arrive from the Admin API as well, and so we handle them the same way */
	response = janus_streaming_process_synchronous_request(session, root);
	if(response != NULL) {
		/* We got a response, send it back */
		goto plugin_response;
	} else if(!strcasecmp(request_text, "watch") || !strcasecmp(request_text, "start")
			|| !strcasecmp(request_text, "pause") || !strcasecmp(request_text, "stop")
			|| !strcasecmp(request_text, "configure") || !strcasecmp(request_text, "switch")) {
		/* These messages are handled asynchronously */
		janus_streaming_message *msg = g_malloc(sizeof(janus_streaming_message));
		msg->handle = handle;
		msg->transaction = transaction;
		msg->message = root;
		msg->jsep = jsep;

		g_async_queue_push(messages, msg);
		return janus_plugin_result_new(JANUS_PLUGIN_OK_WAIT, NULL, NULL);
	} else {
		JANUS_LOG(LOG_VERB, "Unknown request '%s'\n", request_text);
		error_code = JANUS_STREAMING_ERROR_INVALID_REQUEST;
		g_snprintf(error_cause, 512, "Unknown request '%s'", request_text);
	}

plugin_response:
		{
			if(error_code == 0 && !response) {
				error_code = JANUS_STREAMING_ERROR_UNKNOWN_ERROR;
				g_snprintf(error_cause, 512, "Invalid response");
			}
			if(error_code != 0) {
				/* Prepare JSON error event */
				json_t *event = json_object();
				json_object_set_new(event, "streaming", json_string("event"));
				json_object_set_new(event, "error_code", json_integer(error_code));
				json_object_set_new(event, "error", json_string(error_cause));
				response = event;
			}
			if(root != NULL)
				json_decref(root);
			if(jsep != NULL)
				json_decref(jsep);
			g_free(transaction);

			if(session != NULL)
				janus_refcount_decrease(&session->ref);
			return janus_plugin_result_new(JANUS_PLUGIN_OK, NULL, response);
		}

}

json_t *janus_streaming_handle_admin_message(json_t *message) {
	/* Some requests (e.g., 'create' and 'destroy') can be handled via Admin API */
	int error_code = 0;
	char error_cause[512];
	json_t *response = NULL;

	JANUS_VALIDATE_JSON_OBJECT(message, request_parameters,
		error_code, error_cause, TRUE,
		JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
	if(error_code != 0)
		goto admin_response;
	json_t *request = json_object_get(message, "request");
	const char *request_text = json_string_value(request);
	if((response = janus_streaming_process_synchronous_request(NULL, message)) != NULL) {
		/* We got a response, send it back */
		goto admin_response;
	} else {
		JANUS_LOG(LOG_VERB, "Unknown request '%s'\n", request_text);
		error_code = JANUS_STREAMING_ERROR_INVALID_REQUEST;
		g_snprintf(error_cause, 512, "Unknown request '%s'", request_text);
	}

admin_response:
		{
			if(!response) {
				/* Prepare JSON error event */
				response = json_object();
				json_object_set_new(response, "streaming", json_string("event"));
				json_object_set_new(response, "error_code", json_integer(error_code));
				json_object_set_new(response, "error", json_string(error_cause));
			}
			return response;
		}

}

void janus_streaming_setup_media(janus_plugin_session *handle) {
	JANUS_LOG(LOG_INFO, "[%s-%p] WebRTC media is now available\n", JANUS_STREAMING_PACKAGE, handle);
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_mutex_lock(&sessions_mutex);
	janus_streaming_session *session = janus_streaming_lookup_session(handle);
	if(!session) {
		janus_mutex_unlock(&sessions_mutex);
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}
	if(g_atomic_int_get(&session->destroyed)) {
		janus_mutex_unlock(&sessions_mutex);
		return;
	}
	janus_refcount_increase(&session->ref);
	janus_mutex_unlock(&sessions_mutex);
	g_atomic_int_set(&session->hangingup, 0);
	/* We only start streaming towards this user when we get this event */
	janus_rtp_switching_context_reset(&session->context);
	/* If this is related to a live RTP mountpoint, any keyframe we can shoot already? */
	janus_streaming_mountpoint *mountpoint = session->mountpoint;
	if (!mountpoint) {
		janus_refcount_decrease(&session->ref);
		JANUS_LOG(LOG_ERR, "No mountpoint associated with this session...\n");
		return;
	}
	if(mountpoint->streaming_source == janus_streaming_source_rtp) {
		janus_streaming_rtp_source *source = mountpoint->source;
		if(source->keyframe.enabled) {
			JANUS_LOG(LOG_HUGE, "Any keyframe to send?\n");
			janus_mutex_lock(&source->keyframe.mutex);
			if(source->keyframe.latest_keyframe != NULL) {
				JANUS_LOG(LOG_HUGE, "Yep! %d packets\n", g_list_length(source->keyframe.latest_keyframe));
				GList *temp = source->keyframe.latest_keyframe;
				while(temp) {
					janus_streaming_relay_rtp_packet(session, temp->data);
					temp = temp->next;
				}
			}
			janus_mutex_unlock(&source->keyframe.mutex);
		}
		if(source->buffermsg) {
			JANUS_LOG(LOG_HUGE, "Any recent datachannel message to send?\n");
			janus_mutex_lock(&source->buffermsg_mutex);
			if(source->last_msg != NULL) {
				JANUS_LOG(LOG_HUGE, "Yep!\n");
				janus_streaming_relay_rtp_packet(session, source->last_msg);
			}
			janus_mutex_unlock(&source->buffermsg_mutex);
		}
		/* If this mountpoint has RTCP support, send a PLI */
		janus_streaming_rtcp_pli_send(source);
	}
	g_atomic_int_set(&session->started, 1);
	/* Prepare JSON event */
	json_t *event = json_object();
	json_object_set_new(event, "streaming", json_string("event"));
	json_t *result = json_object();
	json_object_set_new(result, "status", json_string("started"));
	json_object_set_new(event, "result", result);
	int ret = gateway->push_event(handle, &janus_streaming_plugin, NULL, event, NULL);
	JANUS_LOG(LOG_VERB, "  >> Pushing event: %d (%s)\n", ret, janus_get_api_error(ret));
	json_decref(event);
	janus_refcount_decrease(&session->ref);
}

void janus_streaming_incoming_rtp(janus_plugin_session *handle, janus_plugin_rtp *packet) {
	if(handle == NULL || g_atomic_int_get(&handle->stopped) || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	/* FIXME We don't care about what the browser sends us, we're sendonly */
}

void janus_streaming_incoming_rtcp(janus_plugin_session *handle, janus_plugin_rtcp *packet) {
	if(handle == NULL || g_atomic_int_get(&handle->stopped) || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_streaming_session *session = (janus_streaming_session *)handle->plugin_handle;
	if(!session || g_atomic_int_get(&session->destroyed) || g_atomic_int_get(&session->stopping) ||
			!g_atomic_int_get(&session->started) || g_atomic_int_get(&session->paused))
		return;
	janus_streaming_mountpoint *mp = (janus_streaming_mountpoint *)session->mountpoint;
	if(mp->streaming_source != janus_streaming_source_rtp)
		return;
	janus_streaming_rtp_source *source = (janus_streaming_rtp_source *)mp->source;
	gboolean video = packet->video;
	char *buf = packet->buffer;
	uint16_t len = packet->length;
	if(!video && (source->audio_rtcp_fd > -1) && (source->audio_rtcp_addr.ss_family != 0)) {
		JANUS_LOG(LOG_HUGE, "Got audio RTCP feedback from a viewer: SSRC %"SCNu32"\n",
			janus_rtcp_get_sender_ssrc(buf, len));
		/* FIXME We don't forward RR packets, so what should we check here? */
	} else if(video && (source->video_rtcp_fd > -1) && (source->video_rtcp_addr.ss_family != 0)) {
		JANUS_LOG(LOG_HUGE, "Got video RTCP feedback from a viewer: SSRC %"SCNu32"\n",
			janus_rtcp_get_sender_ssrc(buf, len));
		/* We only relay PLI/FIR and REMB packets, but in a selective way */
		if(janus_rtcp_has_fir(buf, len) || janus_rtcp_has_pli(buf, len)) {
			/* We got a PLI/FIR, pass it along unless we just sent one */
			JANUS_LOG(LOG_HUGE, "  -- Keyframe request\n");
			janus_streaming_rtcp_pli_send(source);
		}
		uint64_t bw = janus_rtcp_get_remb(buf, len);
		if(bw > 0) {
			/* Keep track of this value, if this is the lowest right now */
			JANUS_LOG(LOG_HUGE, "  -- REMB for this PeerConnection: %"SCNu64"\n", bw);
			if((0 == source->lowest_bitrate) || (source->lowest_bitrate > bw))
				source->lowest_bitrate = bw;
		}
	}
}

void janus_streaming_data_ready(janus_plugin_session *handle) {
	if(handle == NULL || g_atomic_int_get(&handle->stopped) ||
			g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized) || !gateway)
		return;
	/* Data channels are writable: we shouldn't send any datachannel message before this happens */
	janus_streaming_session *session = (janus_streaming_session *)handle->plugin_handle;
	if(!session || g_atomic_int_get(&session->destroyed) || g_atomic_int_get(&session->hangingup))
		return;
	if(g_atomic_int_compare_and_exchange(&session->dataready, 0, 1)) {
		JANUS_LOG(LOG_INFO, "[%s-%p] Data channel available\n", JANUS_STREAMING_PACKAGE, handle);
	}
}

void janus_streaming_hangup_media(janus_plugin_session *handle) {
	JANUS_LOG(LOG_INFO, "[%s-%p] No WebRTC media anymore\n", JANUS_STREAMING_PACKAGE, handle);
	janus_mutex_lock(&sessions_mutex);
	janus_streaming_hangup_media_internal(handle);
	janus_mutex_unlock(&sessions_mutex);
}

static void janus_streaming_hangup_media_internal(janus_plugin_session *handle) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_streaming_session *session = janus_streaming_lookup_session(handle);
	if(!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}
	if(g_atomic_int_get(&session->destroyed))
		return;
	if(!g_atomic_int_compare_and_exchange(&session->hangingup, 0, 1))
		return;
	g_atomic_int_set(&session->dataready, 0);
	g_atomic_int_set(&session->stopping, 1);
	g_atomic_int_set(&session->started, 0);
	g_atomic_int_set(&session->paused, 0);
	janus_rtp_switching_context_reset(&session->context);
	janus_rtp_simulcasting_context_reset(&session->sim_context);
	janus_vp8_simulcast_context_reset(&session->vp8_context);
	session->spatial_layer = -1;
	session->target_spatial_layer = 2;	/* FIXME Chrome sends 0, 1 and 2 (if using EnabledByFlag_3SL3TL) */
	session->last_spatial_layer[0] = 0;
	session->last_spatial_layer[1] = 0;
	session->last_spatial_layer[2] = 0;
	session->temporal_layer = -1;
	session->target_temporal_layer = 2;	/* FIXME Chrome sends 0, 1 and 2 */
	session->e2ee = FALSE;
	janus_mutex_lock(&session->mutex);
	janus_streaming_mountpoint *mp = session->mountpoint;
	session->mountpoint = NULL;
	janus_mutex_unlock(&session->mutex);
	if(mp) {
		janus_mutex_lock(&mp->mutex);
		JANUS_LOG(LOG_VERB, "  -- Removing the session from the mountpoint viewers\n");
		if(g_list_find(mp->viewers, session) != NULL) {
			JANUS_LOG(LOG_VERB, "  -- -- Found!\n");
			janus_refcount_decrease(&mp->ref);
			janus_refcount_decrease(&session->ref);
		}
		mp->viewers = g_list_remove_all(mp->viewers, session);
		if(mp->streaming_source == janus_streaming_source_rtp) {
			/* Remove the viewer from the helper threads too, if any */
			if(mp->helper_threads > 0) {
				GList *l = mp->threads;
				while(l) {
					janus_streaming_helper *ht = (janus_streaming_helper *)l->data;
					janus_mutex_lock(&ht->mutex);
					if(g_list_find(ht->viewers, session) != NULL) {
						ht->num_viewers--;
						ht->viewers = g_list_remove_all(ht->viewers, session);
						janus_mutex_unlock(&ht->mutex);
						JANUS_LOG(LOG_VERB, "Removing viewer from helper thread #%d\n", ht->id);
						break;
					}
					janus_mutex_unlock(&ht->mutex);
					l = l->next;
				}
			}
		}
		janus_mutex_unlock(&mp->mutex);
	}
	g_atomic_int_set(&session->hangingup, 0);
}

/* Thread to handle incoming messages */
static void *janus_streaming_handler(void *data) {
	JANUS_LOG(LOG_VERB, "Joining Streaming handler thread\n");
	janus_streaming_message *msg = NULL;
	int error_code = 0;
	char error_cause[512];
	json_t *root = NULL;
	while(g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping)) {
		msg = g_async_queue_pop(messages);
		if(msg == &exit_message)
			break;
		if(msg->handle == NULL) {
			janus_streaming_message_free(msg);
			continue;
		}
		janus_mutex_lock(&sessions_mutex);
		janus_streaming_session *session = janus_streaming_lookup_session(msg->handle);
		if(!session) {
			janus_mutex_unlock(&sessions_mutex);
			JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
			janus_streaming_message_free(msg);
			continue;
		}
		if(g_atomic_int_get(&session->destroyed)) {
			janus_mutex_unlock(&sessions_mutex);
			janus_streaming_message_free(msg);
			continue;
		}
		janus_mutex_unlock(&sessions_mutex);
		/* Handle request */
		error_code = 0;
		root = NULL;
		if(msg->message == NULL) {
			JANUS_LOG(LOG_ERR, "No message??\n");
			error_code = JANUS_STREAMING_ERROR_NO_MESSAGE;
			g_snprintf(error_cause, 512, "%s", "No message??");
			goto error;
		}
		root = msg->message;
		/* Get the request first */
		JANUS_VALIDATE_JSON_OBJECT(root, request_parameters,
			error_code, error_cause, TRUE,
			JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
		if(error_code != 0)
			goto error;
		json_t *request = json_object_get(root, "request");
		const char *request_text = json_string_value(request);
		json_t *result = NULL;
		const char *sdp_type = NULL;
		char *sdp = NULL;
		gboolean do_restart = FALSE;
		/* All these requests can only be handled asynchronously */
		if(!strcasecmp(request_text, "watch")) {
			JANUS_VALIDATE_JSON_OBJECT(root, watch_parameters,
				error_code, error_cause, TRUE,
				JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
			if(error_code != 0)
				goto error;
			if(!string_ids) {
				JANUS_VALIDATE_JSON_OBJECT(root, id_parameters,
					error_code, error_cause, TRUE,
					JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
			} else {
				JANUS_VALIDATE_JSON_OBJECT(root, idstr_parameters,
					error_code, error_cause, TRUE,
					JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
			}
			if(error_code != 0)
				goto error;
			json_t *id = json_object_get(root, "id");
			guint64 id_value = 0;
			char id_num[30], *id_value_str = NULL;
			if(!string_ids) {
				id_value = json_integer_value(id);
				g_snprintf(id_num, sizeof(id_num), "%"SCNu64, id_value);
				id_value_str = id_num;
			} else {
				id_value_str = (char *)json_string_value(id);
			}
			json_t *offer_audio = json_object_get(root, "offer_audio");
			json_t *offer_video = json_object_get(root, "offer_video");
			json_t *offer_data = json_object_get(root, "offer_data");
			json_t *restart = json_object_get(root, "restart");
			do_restart = restart ? json_is_true(restart) : FALSE;
			janus_mutex_lock(&mountpoints_mutex);
			janus_streaming_mountpoint *mp = g_hash_table_lookup(mountpoints,
				string_ids ? (gpointer)id_value_str : (gpointer)&id_value);
			if(mp == NULL) {
				janus_mutex_unlock(&mountpoints_mutex);
				JANUS_LOG(LOG_VERB, "No such mountpoint/stream %s\n", id_value_str);
				error_code = JANUS_STREAMING_ERROR_NO_SUCH_MOUNTPOINT;
				g_snprintf(error_cause, 512, "No such mountpoint/stream %s", id_value_str);
				goto error;
			}
			janus_refcount_increase(&mp->ref);
			/* A secret may be required for this action */
			JANUS_CHECK_SECRET(mp->pin, root, "pin", error_code, error_cause,
				JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT, JANUS_STREAMING_ERROR_UNAUTHORIZED);
			if(error_code != 0) {
				janus_refcount_decrease(&mp->ref);
				janus_mutex_unlock(&mountpoints_mutex);
				goto error;
			}
			janus_mutex_lock(&mp->mutex);
			janus_mutex_lock(&session->mutex);
			janus_mutex_unlock(&mountpoints_mutex);
			/* Check if this is a new viewer, or if an update is taking place (i.e., ICE restart) */
			if(do_restart) {
				/* User asked for an ICE restart: provide a new offer */
				if(!g_atomic_int_compare_and_exchange(&session->renegotiating, 0, 1)) {
					/* Already triggered a renegotiation, and still waiting for an answer */
					janus_mutex_unlock(&session->mutex);
					janus_mutex_unlock(&mp->mutex);
					JANUS_LOG(LOG_ERR, "Already renegotiating mountpoint %s\n", session->mountpoint->id_str);
					error_code = JANUS_STREAMING_ERROR_INVALID_STATE;
					g_snprintf(error_cause, 512, "Already renegotiating mountpoint %s", session->mountpoint->id_str);
					janus_refcount_decrease(&mp->ref);
					goto error;
				}
				janus_refcount_decrease(&mp->ref);
				JANUS_LOG(LOG_VERB, "Request to perform an ICE restart on mountpoint/stream %s subscription\n", id_value_str);
				session->sdp_version++;	/* This needs to be increased when it changes */
				goto done;
			}
			if(session->mountpoint != NULL) {
				if(session->mountpoint != mp) {
					/* Already watching something else */
					janus_refcount_decrease(&mp->ref);
					JANUS_LOG(LOG_ERR, "Already watching mountpoint %s\n", session->mountpoint->id_str);
					error_code = JANUS_STREAMING_ERROR_INVALID_STATE;
					g_snprintf(error_cause, 512, "Already watching mountpoint %s", session->mountpoint->id_str);
					janus_mutex_unlock(&session->mutex);
					janus_mutex_unlock(&mp->mutex);
					goto error;
				} else {
					/* Make sure it's not an API error */
					if(!g_atomic_int_get(&session->started)) {
						/* Can't be a renegotiation, PeerConnection isn't up yet */
						JANUS_LOG(LOG_ERR, "Already watching mountpoint %s\n", session->mountpoint->id_str);
						error_code = JANUS_STREAMING_ERROR_INVALID_STATE;
						g_snprintf(error_cause, 512, "Already watching mountpoint %s", session->mountpoint->id_str);
						janus_refcount_decrease(&mp->ref);
						janus_mutex_unlock(&session->mutex);
						janus_mutex_unlock(&mp->mutex);
						goto error;
					}
					if(!g_atomic_int_compare_and_exchange(&session->renegotiating, 0, 1)) {
						/* Already triggered a renegotiation, and still waiting for an answer */
						JANUS_LOG(LOG_ERR, "Already renegotiating mountpoint %s\n", session->mountpoint->id_str);
						error_code = JANUS_STREAMING_ERROR_INVALID_STATE;
						g_snprintf(error_cause, 512, "Already renegotiating mountpoint %s", session->mountpoint->id_str);
						janus_refcount_decrease(&mp->ref);
						janus_mutex_unlock(&session->mutex);
						janus_mutex_unlock(&mp->mutex);
						goto error;
					}
					/* Simple renegotiation, remove the extra uneeded reference */
					janus_refcount_decrease(&mp->ref);
					JANUS_LOG(LOG_VERB, "Request to update mountpoint/stream %s subscription (no restart)\n", id_value_str);
					session->sdp_version++;	/* This needs to be increased when it changes */
					goto done;
				}
			}
			/* New viewer: we send an offer ourselves */
			JANUS_LOG(LOG_VERB, "Request to watch mountpoint/stream %s\n", id_value_str);
			if(session->mountpoint != NULL || g_list_find(mp->viewers, session) != NULL) {
				janus_mutex_unlock(&session->mutex);
				janus_mutex_unlock(&mp->mutex);
				janus_refcount_decrease(&mp->ref);
				JANUS_LOG(LOG_ERR, "Already watching a stream...\n");
				error_code = JANUS_STREAMING_ERROR_UNKNOWN_ERROR;
				g_snprintf(error_cause, 512, "Already watching a stream");
				goto error;
			}
			g_atomic_int_set(&session->stopping, 0);
			session->mountpoint = mp;
			session->sdp_version = 1;	/* This needs to be increased when it changes */
			session->sdp_sessid = janus_get_real_time();
			/* Check what we should offer */
			session->audio = offer_audio ? json_is_true(offer_audio) : TRUE;	/* True by default */
			if(!mp->audio)
				session->audio = FALSE;	/* ... unless the mountpoint isn't sending any audio */
			session->video = offer_video ? json_is_true(offer_video) : TRUE;	/* True by default */
			if(!mp->video)
				session->video = FALSE;	/* ... unless the mountpoint isn't sending any video */
			session->data = offer_data ? json_is_true(offer_data) : TRUE;	/* True by default */
			if(!mp->data)
				session->data = FALSE;	/* ... unless the mountpoint isn't sending any data */
			if((!mp->audio || !session->audio) &&
					(!mp->video || !session->video) &&
					(!mp->data || !session->data)) {
				session->mountpoint = NULL;
				janus_mutex_unlock(&session->mutex);
				janus_mutex_unlock(&mp->mutex);
				janus_refcount_decrease(&mp->ref);
				JANUS_LOG(LOG_ERR, "Can't offer an SDP with no audio, video or data for this mountpoint\n");
				error_code = JANUS_STREAMING_ERROR_INVALID_REQUEST;
				g_snprintf(error_cause, 512, "Can't offer an SDP with no audio, video or data for this mountpoint");
				goto error;
			}
			if(mp->streaming_type == janus_streaming_type_on_demand) {
				GError *error = NULL;
				char tname[16];
				g_snprintf(tname, sizeof(tname), "mp %s", mp->id_str);
				janus_refcount_increase(&session->ref);
				janus_refcount_increase(&mp->ref);
				g_thread_try_new(tname, &janus_streaming_ondemand_thread, session, &error);
				if(error != NULL) {
					session->mountpoint = NULL;
					janus_mutex_unlock(&session->mutex);
					janus_refcount_decrease(&session->ref);	/* This is for the failed thread */
					janus_mutex_unlock(&mp->mutex);
					janus_refcount_decrease(&mp->ref);		/* This is for the failed thread */
					janus_refcount_decrease(&mp->ref);
					JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the on-demand thread...\n",
						error->code, error->message ? error->message : "??");
					error_code = JANUS_STREAMING_ERROR_UNKNOWN_ERROR;
					g_snprintf(error_cause, 512, "Got error %d (%s) trying to launch the on-demand thread",
						error->code, error->message ? error->message : "??");
					g_error_free(error);
					goto error;
				}
			} else if(mp->streaming_source == janus_streaming_source_rtp) {
				janus_streaming_rtp_source *source = (janus_streaming_rtp_source *)mp->source;
				if(source && source->simulcast) {
					JANUS_VALIDATE_JSON_OBJECT(root, simulcast_parameters,
						error_code, error_cause, TRUE,
						JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
					if(error_code != 0) {
						session->mountpoint = NULL;
						janus_mutex_unlock(&session->mutex);
						janus_mutex_unlock(&mp->mutex);
						janus_refcount_decrease(&mp->ref);
						goto error;
					}
					/* In case this mountpoint is simulcasting, let's aim high by default */
					janus_rtp_switching_context_reset(&session->context);
					janus_rtp_simulcasting_context_reset(&session->sim_context);
					session->sim_context.substream_target = 2;
					session->sim_context.templayer_target = 2;
					janus_vp8_simulcast_context_reset(&session->vp8_context);
					/* Unless the request contains a target for either layer */
					json_t *substream = json_object_get(root, "substream");
					if(substream) {
						session->sim_context.substream_target = json_integer_value(substream);
						JANUS_LOG(LOG_VERB, "Setting video substream to let through (simulcast): %d (was %d)\n",
							session->sim_context.substream_target, session->sim_context.substream);
					}
					json_t *temporal = json_object_get(root, "temporal");
					if(temporal) {
						session->sim_context.templayer_target = json_integer_value(temporal);
						JANUS_LOG(LOG_VERB, "Setting video temporal layer to let through (simulcast): %d (was %d)\n",
							session->sim_context.templayer_target, session->sim_context.templayer);
					}
					/* Check if we need a custom fallback timer for the substream */
					json_t *fallback = json_object_get(root, "fallback");
					if(fallback) {
						JANUS_LOG(LOG_VERB, "Setting fallback timer (simulcast): %lld (was %"SCNu32")\n",
							json_integer_value(fallback) ? json_integer_value(fallback) : 250000,
							session->sim_context.drop_trigger ? session->sim_context.drop_trigger : 250000);
						session->sim_context.drop_trigger = json_integer_value(fallback);
					}
				} else if(source && source->svc) {
					JANUS_VALIDATE_JSON_OBJECT(root, svc_parameters,
						error_code, error_cause, TRUE,
						JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
					if(error_code != 0) {
						session->mountpoint = NULL;
						janus_mutex_unlock(&session->mutex);
						janus_mutex_unlock(&mp->mutex);
						janus_refcount_decrease(&mp->ref);
						goto error;
					}
					/* In case this mountpoint is doing VP9-SVC, let's aim high by default */
					session->spatial_layer = -1;
					session->target_spatial_layer = 2;	/* FIXME Chrome sends 0, 1 and 2 (if using EnabledByFlag_3SL3TL) */
					session->temporal_layer = -1;
					session->target_temporal_layer = 2;	/* FIXME Chrome sends 0, 1 and 2 */
					/* Unless the request contains a target for either layer */
					json_t *spatial = json_object_get(root, "spatial_layer");
					if(spatial) {
						session->target_spatial_layer = json_integer_value(spatial);
						JANUS_LOG(LOG_VERB, "Setting video spatial layer to let through (SVC): %d (was %d)\n",
							session->target_spatial_layer, session->spatial_layer);
					}
					json_t *temporal = json_object_get(root, "temporal_layer");
					if(temporal) {
						session->target_temporal_layer = json_integer_value(temporal);
						JANUS_LOG(LOG_VERB, "Setting video temporal layer to let through (SVC): %d (was %d)\n",
							session->target_temporal_layer, session->temporal_layer);
					}
				}
				/* If this mountpoint is broadcasting end-to-end encrypted media,
				 * add the info to the JSEP offer we'll be sending them */
				session->e2ee = source->e2ee;
			}
			janus_refcount_increase(&session->ref);
done:
			/* Let's prepare an offer now, but let's also check if there's something we need to skip */
			sdp_type = "offer";	/* We're always going to do the offer ourselves, never answer */
			char sdptemp[2048];
			memset(sdptemp, 0, 2048);
			gchar buffer[512];
			memset(buffer, 0, 512);
			g_snprintf(buffer, 512,
				"v=0\r\no=%s %"SCNu64" %"SCNu64" IN IP4 127.0.0.1\r\n",
					"-", session->sdp_sessid, session->sdp_version);
			g_strlcat(sdptemp, buffer, 2048);
			g_snprintf(buffer, 512,
				"s=Mountpoint %s\r\n", mp->id_str);
			g_strlcat(sdptemp, buffer, 2048);
			g_strlcat(sdptemp, "t=0 0\r\n", 2048);
			if(mp->codecs.audio_pt >= 0 && session->audio) {
				/* Add audio line */
				g_snprintf(buffer, 512,
					"m=audio 1 RTP/SAVPF %d\r\n"
					"c=IN IP4 1.1.1.1\r\n",
					mp->codecs.audio_pt);
				g_strlcat(sdptemp, buffer, 2048);
				if(mp->codecs.audio_rtpmap) {
					g_snprintf(buffer, 512,
						"a=rtpmap:%d %s\r\n",
						mp->codecs.audio_pt, mp->codecs.audio_rtpmap);
					g_strlcat(sdptemp, buffer, 2048);
				}
				if(mp->codecs.audio_fmtp) {
					g_snprintf(buffer, 512,
						"a=fmtp:%d %s\r\n",
						mp->codecs.audio_pt, mp->codecs.audio_fmtp);
					g_strlcat(sdptemp, buffer, 2048);
				}
				g_strlcat(sdptemp, "a=sendonly\r\n", 2048);
				g_snprintf(buffer, 512, "a=extmap:%d %s\r\n", 1, JANUS_RTP_EXTMAP_MID);
				g_strlcat(sdptemp, buffer, 2048);
			}
			if(mp->codecs.video_pt > 0 && session->video) {
				/* Add video line */
				g_snprintf(buffer, 512,
					"m=video 1 RTP/SAVPF %d\r\n"
					"c=IN IP4 1.1.1.1\r\n",
					mp->codecs.video_pt);
				g_strlcat(sdptemp, buffer, 2048);
				if(mp->codecs.video_rtpmap) {
					g_snprintf(buffer, 512,
						"a=rtpmap:%d %s\r\n",
						mp->codecs.video_pt, mp->codecs.video_rtpmap);
					g_strlcat(sdptemp, buffer, 2048);
				}
				if(mp->codecs.video_fmtp) {
					g_snprintf(buffer, 512,
						"a=fmtp:%d %s\r\n",
						mp->codecs.video_pt, mp->codecs.video_fmtp);
					g_strlcat(sdptemp, buffer, 2048);
				}
				g_snprintf(buffer, 512,
					"a=rtcp-fb:%d nack\r\n",
					mp->codecs.video_pt);
				g_strlcat(sdptemp, buffer, 2048);
				g_snprintf(buffer, 512,
					"a=rtcp-fb:%d nack pli\r\n",
					mp->codecs.video_pt);
				g_strlcat(sdptemp, buffer, 2048);
				g_snprintf(buffer, 512,
					"a=rtcp-fb:%d goog-remb\r\n",
					mp->codecs.video_pt);
				g_strlcat(sdptemp, buffer, 2048);
				g_strlcat(sdptemp, "a=sendonly\r\n", 2048);
				g_snprintf(buffer, 512, "a=extmap:%d %s\r\n", 1, JANUS_RTP_EXTMAP_MID);
				g_strlcat(sdptemp, buffer, 2048);
			}
#ifdef HAVE_SCTP
			if(mp->data && session->data) {
				/* Add data line */
				g_snprintf(buffer, 512,
					"m=application 1 UDP/DTLS/SCTP webrtc-datachannel\r\n"
					"c=IN IP4 1.1.1.1\r\n"
					"a=sctp-port:5000\r\n");
				g_strlcat(sdptemp, buffer, 2048);
			}
#endif
			sdp = g_strdup(sdptemp);
			JANUS_LOG(LOG_VERB, "Going to %s this SDP:\n%s\n", sdp_type, sdp);
			result = json_object();
			json_object_set_new(result, "status", json_string(do_restart ? "updating" : "preparing"));
			/* Add the user to the list of watchers and we're done */
			if(g_list_find(mp->viewers, session) == NULL) {
				mp->viewers = g_list_append(mp->viewers, session);
				if(mp->streaming_source == janus_streaming_source_rtp) {
					/* If we're using helper threads, add the viewer to one of those */
					if(mp->helper_threads > 0) {
						int viewers = -1;
						janus_streaming_helper *helper = NULL;
						GList *l = mp->threads;
						while(l) {
							janus_streaming_helper *ht = (janus_streaming_helper *)l->data;
							if(viewers == -1 || (helper == NULL && ht->num_viewers == 0) || ht->num_viewers < viewers) {
								viewers = ht->num_viewers;
								helper = ht;
							}
							l = l->next;
						}
						janus_mutex_lock(&helper->mutex);
						helper->viewers = g_list_append(helper->viewers, session);
						helper->num_viewers++;
						janus_mutex_unlock(&helper->mutex);
						JANUS_LOG(LOG_VERB, "Added viewer to helper thread #%d (%d viewers)\n",
							helper->id, helper->num_viewers);
					}
				}
			}
			janus_mutex_unlock(&session->mutex);
			janus_mutex_unlock(&mp->mutex);
		} else if(!strcasecmp(request_text, "start")) {
			if(session->mountpoint == NULL) {
				JANUS_LOG(LOG_VERB, "Can't start: no mountpoint set\n");
				error_code = JANUS_STREAMING_ERROR_NO_SUCH_MOUNTPOINT;
				g_snprintf(error_cause, 512, "Can't start: no mountpoint set");
				goto error;
			}
			JANUS_LOG(LOG_VERB, "Starting the streaming\n");
			g_atomic_int_set(&session->paused, 0);
			result = json_object();
			/* We wait for the setup_media event to start: on the other hand, it may have already arrived */
			json_object_set_new(result, "status", json_string(g_atomic_int_get(&session->started) ? "started" : "starting"));
			/* Also notify event handlers */
			if(notify_events && gateway->events_is_enabled()) {
				json_t *info = json_object();
				json_object_set_new(info, "status", json_string("starting"));
				if(session->mountpoint != NULL)
					json_object_set_new(info, "id", string_ids ?
						json_string(session->mountpoint->id_str) :json_integer(session->mountpoint->id));
				gateway->notify_event(&janus_streaming_plugin, session->handle, info);
			}
		} else if(!strcasecmp(request_text, "pause")) {
			if(session->mountpoint == NULL) {
				JANUS_LOG(LOG_VERB, "Can't pause: no mountpoint set\n");
				error_code = JANUS_STREAMING_ERROR_NO_SUCH_MOUNTPOINT;
				g_snprintf(error_cause, 512, "Can't start: no mountpoint set");
				goto error;
			}
			JANUS_LOG(LOG_VERB, "Pausing the streaming\n");
			g_atomic_int_set(&session->paused, 1);
			result = json_object();
			json_object_set_new(result, "status", json_string("pausing"));
			/* Also notify event handlers */
			if(notify_events && gateway->events_is_enabled()) {
				json_t *info = json_object();
				json_object_set_new(info, "status", json_string("pausing"));
				if(session->mountpoint != NULL)
					json_object_set_new(info, "id", string_ids ?
						json_string(session->mountpoint->id_str) : json_integer(session->mountpoint->id));
				gateway->notify_event(&janus_streaming_plugin, session->handle, info);
			}
		} else if(!strcasecmp(request_text, "configure")) {
			janus_streaming_mountpoint *mp = session->mountpoint;
			if(mp == NULL) {
				JANUS_LOG(LOG_VERB, "Can't configure: not on a mountpoint\n");
				error_code = JANUS_STREAMING_ERROR_NO_SUCH_MOUNTPOINT;
				g_snprintf(error_cause, 512, "Can't configure: not on a mountpoint");
				goto error;
			}
			JANUS_VALIDATE_JSON_OBJECT(root, configure_parameters,
				error_code, error_cause, TRUE,
				JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
			json_t *audio = json_object_get(root, "audio");
			if(audio)
				session->audio = json_is_true(audio);
			json_t *video = json_object_get(root, "video");
			if(video)
				session->video = json_is_true(video);
			json_t *data = json_object_get(root, "data");
			if(data)
				session->data = json_is_true(data);
			if(mp->streaming_source == janus_streaming_source_rtp) {
				janus_streaming_rtp_source *source = (janus_streaming_rtp_source *)mp->source;
				if(source && source->simulcast) {
					/* Check if the viewer is requesting a different substream/temporal layer */
					json_t *substream = json_object_get(root, "substream");
					if(substream) {
						session->sim_context.substream_target = json_integer_value(substream);
						JANUS_LOG(LOG_VERB, "Setting video substream to let through (simulcast): %d (was %d)\n",
							session->sim_context.substream_target, session->sim_context.substream);
						if(session->sim_context.substream_target == session->sim_context.substream) {
							/* No need to do anything, we're already getting the right substream, so notify the viewer */
							json_t *event = json_object();
							json_object_set_new(event, "streaming", json_string("event"));
							json_t *result = json_object();
							json_object_set_new(result, "substream", json_integer(session->sim_context.substream));
							json_object_set_new(event, "result", result);
							gateway->push_event(session->handle, &janus_streaming_plugin, NULL, event, NULL);
							json_decref(event);
						} else {
							/* Schedule a PLI */
							JANUS_LOG(LOG_VERB, "We need a PLI for the simulcast context\n");
							g_atomic_int_set(&source->need_pli, 1);
						}
					}
					json_t *temporal = json_object_get(root, "temporal");
					if(temporal) {
						session->sim_context.templayer_target = json_integer_value(temporal);
						JANUS_LOG(LOG_VERB, "Setting video temporal layer to let through (simulcast): %d (was %d)\n",
							session->sim_context.templayer_target, session->sim_context.templayer);
						if(mp->codecs.video_codec == JANUS_VIDEOCODEC_VP8 && session->sim_context.templayer_target == session->sim_context.templayer) {
							/* No need to do anything, we're already getting the right temporal layer, so notify the viewer */
							json_t *event = json_object();
							json_object_set_new(event, "streaming", json_string("event"));
							json_t *result = json_object();
							json_object_set_new(result, "temporal", json_integer(session->sim_context.templayer));
							json_object_set_new(event, "result", result);
							gateway->push_event(session->handle, &janus_streaming_plugin, NULL, event, NULL);
							json_decref(event);
						}
					}
					/* Check if we need to change the fallback timer for the substream */
					json_t *fallback = json_object_get(root, "fallback");
					if(fallback) {
						JANUS_LOG(LOG_VERB, "Setting fallback timer (simulcast): %lld (was %"SCNu32")\n",
							json_integer_value(fallback) ? json_integer_value(fallback) : 250000,
							session->sim_context.drop_trigger ? session->sim_context.drop_trigger : 250000);
						session->sim_context.drop_trigger = json_integer_value(fallback);
					}
				}
				if(source && source->svc) {
					/* Check if the viewer is requesting a different SVC spatial/temporal layer */
					json_t *spatial = json_object_get(root, "spatial_layer");
					if(spatial) {
						int spatial_layer = json_integer_value(spatial);
						if(spatial_layer > 1) {
							JANUS_LOG(LOG_WARN, "Spatial layer higher than 1, will probably be ignored\n");
						}
						if(spatial_layer == session->spatial_layer) {
							/* No need to do anything, we're already getting the right spatial layer, so notify the user */
							json_t *event = json_object();
							json_object_set_new(event, "streaming", json_string("event"));
							json_t *result = json_object();
							json_object_set_new(result, "spatial_layer", json_integer(session->spatial_layer));
							json_object_set_new(event, "result", result);
							gateway->push_event(msg->handle, &janus_streaming_plugin, NULL, event, NULL);
							json_decref(event);
						} else if(spatial_layer != session->target_spatial_layer) {
							/* Send a FIR to the source, if RTCP is enabled */
							g_atomic_int_set(&source->need_pli, 1);
						}
						session->target_spatial_layer = spatial_layer;
					}
					json_t *temporal = json_object_get(root, "temporal_layer");
					if(temporal) {
						int temporal_layer = json_integer_value(temporal);
						if(temporal_layer > 2) {
							JANUS_LOG(LOG_WARN, "Temporal layer higher than 2, will probably be ignored\n");
						}
						if(temporal_layer == session->temporal_layer) {
							/* No need to do anything, we're already getting the right temporal layer, so notify the user */
							json_t *event = json_object();
							json_object_set_new(event, "streaming", json_string("event"));
							json_t *result = json_object();
							json_object_set_new(result, "temporal_layer", json_integer(session->temporal_layer));
							json_object_set_new(event, "result", result);
							gateway->push_event(msg->handle, &janus_streaming_plugin, NULL, event, NULL);
							json_decref(event);
						}
						session->target_temporal_layer = temporal_layer;
					}
				}
			}
			/* Done */
			result = json_object();
			json_object_set_new(result, "event", json_string("configured"));
		} else if(!strcasecmp(request_text, "switch")) {
			/* This listener wants to switch to a different mountpoint
			 * NOTE: this only works for live RTP streams as of now: you
			 * cannot, for instance, switch from a live RTP mountpoint to
			 * an on demand one or viceversa (TBD.) */
			janus_mutex_lock(&session->mutex);
			janus_streaming_mountpoint *oldmp = session->mountpoint;
			if(oldmp == NULL) {
				janus_mutex_unlock(&session->mutex);
				JANUS_LOG(LOG_VERB, "Can't switch: not on a mountpoint\n");
				error_code = JANUS_STREAMING_ERROR_NO_SUCH_MOUNTPOINT;
				g_snprintf(error_cause, 512, "Can't switch: not on a mountpoint");
				goto error;
			}
			if(oldmp->streaming_type != janus_streaming_type_live ||
					oldmp->streaming_source != janus_streaming_source_rtp) {
				janus_mutex_unlock(&session->mutex);
				JANUS_LOG(LOG_VERB, "Can't switch: not on a live RTP mountpoint\n");
				error_code = JANUS_STREAMING_ERROR_CANT_SWITCH;
				g_snprintf(error_cause, 512, "Can't switch: not on a live RTP mountpoint");
				goto error;
			}
			janus_refcount_increase(&oldmp->ref);
			if(!string_ids) {
				JANUS_VALIDATE_JSON_OBJECT(root, id_parameters,
					error_code, error_cause, TRUE,
					JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
			} else {
				JANUS_VALIDATE_JSON_OBJECT(root, idstr_parameters,
					error_code, error_cause, TRUE,
					JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
			}
			if(error_code != 0) {
				janus_mutex_unlock(&session->mutex);
				janus_refcount_decrease(&oldmp->ref);
				goto error;
			}
			json_t *id = json_object_get(root, "id");
			guint64 id_value = 0;
			char id_num[30], *id_value_str = NULL;
			if(!string_ids) {
				id_value = json_integer_value(id);
				g_snprintf(id_num, sizeof(id_num), "%"SCNu64, id_value);
				id_value_str = id_num;
			} else {
				id_value_str = (char *)json_string_value(id);
			}
			janus_mutex_lock(&mountpoints_mutex);
			janus_streaming_mountpoint *mp = g_hash_table_lookup(mountpoints,
				string_ids ? (gpointer)id_value_str : (gpointer)&id_value);
			if(mp == NULL) {
				janus_mutex_unlock(&mountpoints_mutex);
				janus_mutex_unlock(&session->mutex);
				JANUS_LOG(LOG_VERB, "No such mountpoint/stream %s\n", id_value_str);
				error_code = JANUS_STREAMING_ERROR_NO_SUCH_MOUNTPOINT;
				g_snprintf(error_cause, 512, "No such mountpoint/stream %s", id_value_str);
				goto error;
			}
			janus_refcount_increase(&mp->ref);	/* If the switch succeeds, we don't decrease this now */
			if(mp->streaming_type != janus_streaming_type_live ||
					mp->streaming_source != janus_streaming_source_rtp) {
				janus_refcount_decrease(&oldmp->ref);
				janus_refcount_decrease(&mp->ref);
				janus_mutex_unlock(&mountpoints_mutex);
				janus_mutex_unlock(&session->mutex);
				JANUS_LOG(LOG_VERB, "Can't switch: target is not a live RTP mountpoint\n");
				error_code = JANUS_STREAMING_ERROR_CANT_SWITCH;
				g_snprintf(error_cause, 512, "Can't switch: target is not a live RTP mountpoint");
				goto error;
			}
			janus_streaming_rtp_source *source = (janus_streaming_rtp_source *)mp->source;
			if(source && source->simulcast) {
				JANUS_VALIDATE_JSON_OBJECT(root, simulcast_parameters,
					error_code, error_cause, TRUE,
					JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
				if(error_code != 0) {
					janus_refcount_decrease(&oldmp->ref);
					janus_refcount_decrease(&mp->ref);
					janus_mutex_unlock(&mountpoints_mutex);
					janus_mutex_unlock(&session->mutex);
					goto error;
				}
				/* In case this mountpoint is simulcasting, let's aim high by default */
				janus_rtp_simulcasting_context_reset(&session->sim_context);
				session->sim_context.substream_target = 2;
				session->sim_context.templayer_target = 2;
				janus_vp8_simulcast_context_reset(&session->vp8_context);
				/* Unless the request contains a target for either layer */
				json_t *substream = json_object_get(root, "substream");
				if(substream) {
					session->sim_context.substream_target = json_integer_value(substream);
					JANUS_LOG(LOG_VERB, "Setting video substream to let through (simulcast): %d (was %d)\n",
						session->sim_context.substream_target, session->sim_context.substream);
				}
				json_t *temporal = json_object_get(root, "temporal");
				if(temporal) {
					session->sim_context.templayer_target = json_integer_value(temporal);
					JANUS_LOG(LOG_VERB, "Setting video temporal layer to let through (simulcast): %d (was %d)\n",
						session->sim_context.templayer_target, session->sim_context.templayer);
				}
				/* Check if we need a custom fallback timer for the substream */
				json_t *fallback = json_object_get(root, "fallback");
				if(fallback) {
					JANUS_LOG(LOG_VERB, "Setting fallback timer (simulcast): %lld (was %"SCNu32")\n",
						json_integer_value(fallback) ? json_integer_value(fallback) : 250000,
						session->sim_context.drop_trigger ? session->sim_context.drop_trigger : 250000);
					session->sim_context.drop_trigger = json_integer_value(fallback);
				}
			} else if(source && source->svc) {
				JANUS_VALIDATE_JSON_OBJECT(root, svc_parameters,
					error_code, error_cause, TRUE,
					JANUS_STREAMING_ERROR_MISSING_ELEMENT, JANUS_STREAMING_ERROR_INVALID_ELEMENT);
				if(error_code != 0) {
					janus_refcount_decrease(&oldmp->ref);
					janus_refcount_decrease(&mp->ref);
					janus_mutex_unlock(&mountpoints_mutex);
					janus_mutex_unlock(&session->mutex);
					goto error;
				}
				/* In case this mountpoint is doing VP9-SVC, let's aim high by default */
				session->spatial_layer = -1;
				session->target_spatial_layer = 2;	/* FIXME Chrome sends 0, 1 and 2 (if using EnabledByFlag_3SL3TL) */
				session->temporal_layer = -1;
				session->target_temporal_layer = 2;	/* FIXME Chrome sends 0, 1 and 2 */
				/* Unless the request contains a target for either layer */
				json_t *spatial = json_object_get(root, "spatial_layer");
				if(spatial) {
					session->target_spatial_layer = json_integer_value(spatial);
					JANUS_LOG(LOG_VERB, "Setting video spatial layer to let through (SVC): %d (was %d)\n",
						session->target_spatial_layer, session->spatial_layer);
				}
				json_t *temporal = json_object_get(root, "temporal_layer");
				if(temporal) {
					session->target_temporal_layer = json_integer_value(temporal);
					JANUS_LOG(LOG_VERB, "Setting video temporal layer to let through (SVC): %d (was %d)\n",
						session->target_temporal_layer, session->temporal_layer);
				}
			}
			janus_mutex_unlock(&mountpoints_mutex);
			JANUS_LOG(LOG_VERB, "Request to switch to mountpoint/stream %s (old: %s)\n", mp->id_str, oldmp->id_str);
			g_atomic_int_set(&session->paused, 1);
			/* Unsubscribe from the previous mountpoint and subscribe to the new one */
			session->mountpoint = NULL;
			janus_mutex_unlock(&session->mutex);
			janus_mutex_lock(&oldmp->mutex);
			oldmp->viewers = g_list_remove_all(oldmp->viewers, session);
			/* Remove the viewer from the helper threads too, if any */
			if(oldmp->helper_threads > 0) {
				GList *l = oldmp->threads;
				while(l) {
					janus_streaming_helper *ht = (janus_streaming_helper *)l->data;
					janus_mutex_lock(&ht->mutex);
					if(g_list_find(ht->viewers, session) != NULL) {
						ht->num_viewers--;
						ht->viewers = g_list_remove_all(ht->viewers, session);
						janus_mutex_unlock(&ht->mutex);
						JANUS_LOG(LOG_VERB, "Removing viewer from helper thread #%d (switching)\n", ht->id);
						break;
					}
					janus_mutex_unlock(&ht->mutex);
					l = l->next;
				}
			}
			janus_refcount_decrease(&oldmp->ref);	/* This is for the user going away */
			janus_mutex_unlock(&oldmp->mutex);
			/* Subscribe to the new one */
			janus_mutex_lock(&mp->mutex);
			janus_mutex_lock(&session->mutex);
			mp->viewers = g_list_append(mp->viewers, session);
			/* If we're using helper threads, add the viewer to one of those */
			if(mp->helper_threads > 0) {
				int viewers = 0;
				janus_streaming_helper *helper = NULL;
				GList *l = mp->threads;
				while(l) {
					janus_streaming_helper *ht = (janus_streaming_helper *)l->data;
					if(ht->num_viewers == 0 || ht->num_viewers < viewers) {
						viewers = ht->num_viewers;
						helper = ht;
					}
					l = l->next;
				}
				JANUS_LOG(LOG_VERB, "Adding viewer to helper thread #%d\n", helper->id);
				janus_mutex_lock(&helper->mutex);
				helper->viewers = g_list_append(helper->viewers, session);
				helper->num_viewers++;
				janus_mutex_unlock(&helper->mutex);
			}
			session->mountpoint = mp;
			g_atomic_int_set(&session->paused, 0);
			janus_mutex_unlock(&session->mutex);
			janus_mutex_unlock(&mp->mutex);
			/* Done */
			janus_refcount_decrease(&oldmp->ref);	/* This is for the request being done with it */
			result = json_object();
			json_object_set_new(result, "switched", json_string("ok"));
			json_object_set_new(result, "id", string_ids ? json_string(id_value_str) : json_integer(id_value));
			/* Also notify event handlers */
			if(notify_events && gateway->events_is_enabled()) {
				json_t *info = json_object();
				json_object_set_new(info, "status", json_string("switching"));
				json_object_set_new(info, "id", string_ids ? json_string(id_value_str) : json_integer(id_value));
				gateway->notify_event(&janus_streaming_plugin, session->handle, info);
			}
		} else if(!strcasecmp(request_text, "stop")) {
			if(g_atomic_int_get(&session->stopping) || !g_atomic_int_get(&session->started)) {
				/* Been there, done that: ignore */
				janus_streaming_message_free(msg);
				continue;
			}
			JANUS_LOG(LOG_VERB, "Stopping the streaming\n");
			result = json_object();
			json_object_set_new(result, "status", json_string("stopping"));
			/* Also notify event handlers */
			if(notify_events && gateway->events_is_enabled()) {
				json_t *info = json_object();
				json_object_set_new(info, "status", json_string("stopping"));
				janus_streaming_mountpoint *mp = session->mountpoint;
				if(mp)
					json_object_set_new(info, "id", string_ids ? json_string(mp->id_str) : json_integer(mp->id));
				gateway->notify_event(&janus_streaming_plugin, session->handle, info);
			}
			/* Tell the core to tear down the PeerConnection, hangup_media will do the rest */
			gateway->close_pc(session->handle);
		} else {
			JANUS_LOG(LOG_VERB, "Unknown request '%s'\n", request_text);
			error_code = JANUS_STREAMING_ERROR_INVALID_REQUEST;
			g_snprintf(error_cause, 512, "Unknown request '%s'", request_text);
			goto error;
		}

		/* Any SDP to handle? */
		const char *msg_sdp_type = json_string_value(json_object_get(msg->jsep, "type"));
		const char *msg_sdp = json_string_value(json_object_get(msg->jsep, "sdp"));
		if(msg_sdp) {
			JANUS_LOG(LOG_VERB, "This is involving a negotiation (%s) as well (%s):\n%s\n",
				do_restart ? "renegotiation occurring" : "but we really don't care", msg_sdp_type, msg_sdp);
		}
		g_atomic_int_set(&session->renegotiating, 0);

		/* Prepare JSON event */
		json_t *jsep = json_pack("{ssss}", "type", sdp_type, "sdp", sdp);
		if(do_restart)
			json_object_set_new(jsep, "restart", json_true());
		if(session->e2ee)
			json_object_set_new(jsep, "e2ee", json_true());
		json_t *event = json_object();
		json_object_set_new(event, "streaming", json_string("event"));
		if(result != NULL)
			json_object_set_new(event, "result", result);
		int ret = gateway->push_event(msg->handle, &janus_streaming_plugin, msg->transaction, event, jsep);
		JANUS_LOG(LOG_VERB, "  >> Pushing event: %d (%s)\n", ret, janus_get_api_error(ret));
		g_free(sdp);
		json_decref(event);
		json_decref(jsep);
		janus_streaming_message_free(msg);
		continue;

error:
		{
			/* Prepare JSON error event */
			json_t *event = json_object();
			json_object_set_new(event, "streaming", json_string("event"));
			json_object_set_new(event, "error_code", json_integer(error_code));
			json_object_set_new(event, "error", json_string(error_cause));
			int ret = gateway->push_event(msg->handle, &janus_streaming_plugin, msg->transaction, event, NULL);
			JANUS_LOG(LOG_VERB, "  >> Pushing event: %d (%s)\n", ret, janus_get_api_error(ret));
			json_decref(event);
			janus_streaming_message_free(msg);
		}
	}
	JANUS_LOG(LOG_VERB, "Leaving Streaming handler thread\n");
	return NULL;
}

/* Helpers to create a listener filedescriptor */
static int janus_streaming_create_fd(int port, in_addr_t mcast, const janus_network_address *iface, char *host, size_t hostlen,
		const char *listenername, const char *medianame, const char *mountpointname, gboolean quiet) {
	janus_mutex_lock(&fd_mutex);
	struct sockaddr_in address = { 0 };
	struct sockaddr_in6 address6 = { 0 };
	janus_network_address_string_buffer address_representation;

	uint16_t rtp_port_next = rtp_range_slider; 					/* Read global slider */
	uint16_t rtp_port_start = rtp_port_next;
	gboolean use_range = (port == 0), rtp_port_wrap = FALSE;

	int fd = -1, family = 0;
	while(1) {
		family = 0;	/* By default, we bind to both IPv4 and IPv6 */
		if(use_range && rtp_port_wrap && rtp_port_next >= rtp_port_start) {
			/* Full range scanned */
			JANUS_LOG(LOG_ERR, "No ports available for RTP/RTCP in range: %u -- %u\n",
				  rtp_range_min, rtp_range_max);
			break;
		}
		if(!use_range) {
			/* Use the port specified in the arguments */
			if(IN_MULTICAST(ntohl(mcast))) {
				fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
				if(fd < 0) {
					JANUS_LOG(LOG_ERR, "[%s] Cannot create socket for %s... %d (%s)\n",
						mountpointname, medianame, errno, strerror(errno));
					break;
				}
#ifdef IP_MULTICAST_ALL
				int mc_all = 0;
				if((setsockopt(fd, IPPROTO_IP, IP_MULTICAST_ALL, (void*) &mc_all, sizeof(mc_all))) < 0) {
					JANUS_LOG(LOG_ERR, "[%s] %s listener setsockopt IP_MULTICAST_ALL failed... %d (%s)\n",
						mountpointname, listenername, errno, strerror(errno));
					close(fd);
					janus_mutex_unlock(&fd_mutex);
					return -1;
				}
#endif
				struct ip_mreq mreq;
				memset(&mreq, '\0', sizeof(mreq));
				mreq.imr_multiaddr.s_addr = mcast;
				if(!janus_network_address_is_null(iface)) {
					family = AF_INET;
					if(iface->family == AF_INET) {
						mreq.imr_interface = iface->ipv4;
						(void) janus_network_address_to_string_buffer(iface, &address_representation); /* This is OK: if we get here iface must be non-NULL */
						char *maddr = inet_ntoa(mreq.imr_multiaddr);
						JANUS_LOG(LOG_INFO, "[%s] %s listener using interface address: %s (%s)\n", mountpointname, listenername,
							janus_network_address_string_from_buffer(&address_representation), maddr);
						if(maddr && host && hostlen > 0)
							g_strlcpy(host, maddr, hostlen);
					} else {
						JANUS_LOG(LOG_ERR, "[%s] %s listener: invalid multicast address type (only IPv4 multicast is currently supported by this plugin)\n", mountpointname, listenername);
						close(fd);
						janus_mutex_unlock(&fd_mutex);
						return -1;
					}
				} else {
					JANUS_LOG(LOG_WARN, "[%s] No multicast interface for: %s. This may not work as expected if you have multiple network devices (NICs)\n", mountpointname, listenername);
				}
				if(setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) == -1) {
					JANUS_LOG(LOG_ERR, "[%s] %s listener IP_ADD_MEMBERSHIP failed... %d (%s)\n",
						mountpointname, listenername, errno, strerror(errno));
					close(fd);
					janus_mutex_unlock(&fd_mutex);
					return -1;
				}
				JANUS_LOG(LOG_INFO, "[%s] %s listener IP_ADD_MEMBERSHIP ok\n", mountpointname, listenername);
			}
		} else {
			/* Pick a port in the configured range */
			port = rtp_port_next;
			if((uint32_t)(rtp_port_next) < rtp_range_max) {
				rtp_port_next++;
			} else {
				rtp_port_next = rtp_range_min;
				rtp_port_wrap = TRUE;
			}
		}
		address.sin_family = AF_INET;
		address.sin_port = htons(port);
		address.sin_addr.s_addr = INADDR_ANY;
		address6.sin6_family = AF_INET6;
		address6.sin6_port = htons(port);
		address6.sin6_addr = in6addr_any;
		/* If this is multicast, allow a re-use of the same ports (different groups may be used) */
		if(!use_range && IN_MULTICAST(ntohl(mcast))) {
			int reuse = 1;
			if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) {
				JANUS_LOG(LOG_ERR, "[%s] %s listener setsockopt SO_REUSEADDR failed... %d (%s)\n",
					mountpointname, listenername, errno, strerror(errno));
				close(fd);
				janus_mutex_unlock(&fd_mutex);
				return -1;
			}
			/* TODO IPv6 */
			family = AF_INET;
			address.sin_addr.s_addr = mcast;
		} else {
			if(!IN_MULTICAST(ntohl(mcast)) && !janus_network_address_is_null(iface)) {
				family = iface->family;
				if(iface->family == AF_INET) {
					address.sin_addr = iface->ipv4;
					(void) janus_network_address_to_string_buffer(iface, &address_representation); /* This is OK: if we get here iface must be non-NULL */
					JANUS_LOG(LOG_INFO, "[%s] %s listener restricted to interface address: %s\n",
						mountpointname, listenername, janus_network_address_string_from_buffer(&address_representation));
					if(host && hostlen > 0)
						g_strlcpy(host, janus_network_address_string_from_buffer(&address_representation), hostlen);
				} else if(iface->family == AF_INET6) {
					memcpy(&address6.sin6_addr, &iface->ipv6, sizeof(iface->ipv6));
					(void) janus_network_address_to_string_buffer(iface, &address_representation); /* This is OK: if we get here iface must be non-NULL */
					JANUS_LOG(LOG_INFO, "[%s] %s listener restricted to interface address: %s\n",
						mountpointname, listenername, janus_network_address_string_from_buffer(&address_representation));
					if(host && hostlen > 0)
						g_strlcpy(host, janus_network_address_string_from_buffer(&address_representation), hostlen);
				} else {
					JANUS_LOG(LOG_ERR, "[%s] %s listener: invalid address/restriction type\n", mountpointname, listenername);
					continue;
				}
			}
		}
		/* Bind to the specified port */
		if(fd == -1) {
			fd = socket(family == AF_INET ? AF_INET : AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
			int v6only = 0;
			if(fd < 0) {
				JANUS_LOG(LOG_ERR, "[%s] Cannot create socket for %s... %d (%s)\n",
					mountpointname, medianame, errno, strerror(errno));
				break;
			}
			if(family != AF_INET && setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) != 0) {
				JANUS_LOG(LOG_ERR, "[%s] setsockopt on socket failed for %s... %d (%s)\n",
					mountpointname, medianame, errno, strerror(errno));
				break;
			}
		}
		size_t addrlen = (family == AF_INET ? sizeof(address) : sizeof(address6));
		if(bind(fd, (family == AF_INET ? (struct sockaddr *)&address : (struct sockaddr *)&address6), addrlen) < 0) {
			close(fd);
			fd = -1;
			if(!quiet) {
				JANUS_LOG(LOG_ERR, "[%s] Bind failed for %s (port %d)... %d (%s)\n",
					mountpointname, medianame, port, errno, strerror(errno));
			}
			if(!use_range)	/* Asked for a specific port but it's not available, give up */
				break;
		} else {
			if(use_range)
				rtp_range_slider = port;	/* Update global slider */
			break;
		}
	}
	janus_mutex_unlock(&fd_mutex);
	return fd;
}
/* Helper to bind RTP/RTCP port pair (for RTSP) */
static int janus_streaming_allocate_port_pair(const char *name, const char *media,
		in_addr_t mcast, const janus_network_address *iface, multiple_fds *fds, int ports[2]) {
	/* Start from the global slider */
	uint16_t rtp_port_next = rtp_range_slider;
	if(rtp_port_next % 2 != 0)	/* We want an even port for RTP */
		rtp_port_next++;
	uint16_t rtp_port_start = rtp_port_next;
	gboolean rtp_port_wrap = FALSE;

	int rtp_fd = -1, rtcp_fd = -1;
	while(1) {
		if(rtp_port_wrap && rtp_port_next >= rtp_port_start) {
			/* Full range scanned */
			JANUS_LOG(LOG_ERR, "No ports available for audio/video channel in range: %u -- %u\n",
				rtp_range_min, rtp_range_max);
			break;
		}
		int rtp_port = rtp_port_next;
		int rtcp_port = rtp_port+1;
		if((uint32_t)(rtp_port_next + 2UL) < rtp_range_max) {
			/* Advance to next pair */
			rtp_port_next += 2;
		} else {
			rtp_port_next = rtp_range_min;
			rtp_port_wrap = TRUE;
		}
		rtp_fd = janus_streaming_create_fd(rtp_port, mcast, iface, NULL, 0, media, media, name, TRUE);
		if(rtp_fd != -1) {
			rtcp_fd = janus_streaming_create_fd(rtcp_port, mcast, iface, NULL, 0, media, media, name, TRUE);
			if(rtcp_fd != -1) {
				/* Done */
				fds->fd = rtp_fd;
				fds->rtcp_fd = rtcp_fd;
				ports[0] = rtp_port;
				ports[1] = rtcp_port;
				/* Update global slider */
				rtp_range_slider = rtp_port_next;
				return 0;
			}
		}
		/* If we got here, something failed: try again */
		if(rtp_fd != -1)
			close(rtp_fd);
	}
	return -1;
}

/* Helper to return fd port */
static int janus_streaming_get_fd_port(int fd) {
	struct sockaddr_in6 server = { 0 };
	socklen_t len = sizeof(server);
	if(getsockname(fd, &server, &len) == -1) {
		return -1;
	}

	return ntohs(server.sin6_port);
}

/* Helpers to destroy a streaming mountpoint. */
static void janus_streaming_rtp_source_free(janus_streaming_rtp_source *source) {
	if(source->audio_fd > -1) {
		close(source->audio_fd);
	}
	if(source->video_fd[0] > -1) {
		close(source->video_fd[0]);
	}
	if(source->video_fd[1] > -1) {
		close(source->video_fd[1]);
	}
	if(source->video_fd[2] > -1) {
		close(source->video_fd[2]);
	}
	if(source->data_fd > -1) {
		close(source->data_fd);
	}
	if(source->audio_rtcp_fd > -1) {
		close(source->audio_rtcp_fd);
	}
	if(source->video_rtcp_fd > -1) {
		close(source->video_rtcp_fd);
	}
	if(source->pipefd[0] > -1) {
		close(source->pipefd[0]);
	}
	if(source->pipefd[1] > -1) {
		close(source->pipefd[1]);
	}
	g_free(source->audio_host);
	g_free(source->video_host);
	g_free(source->data_host);
	janus_mutex_lock(&source->keyframe.mutex);
	if(source->keyframe.latest_keyframe != NULL)
		g_list_free_full(source->keyframe.latest_keyframe, (GDestroyNotify)janus_streaming_rtp_relay_packet_free);
	source->keyframe.latest_keyframe = NULL;
	janus_mutex_unlock(&source->keyframe.mutex);
	janus_mutex_lock(&source->buffermsg_mutex);
	if(source->last_msg != NULL)
		janus_streaming_rtp_relay_packet_free((janus_streaming_rtp_relay_packet *)source->last_msg);
	source->last_msg = NULL;
	janus_mutex_unlock(&source->buffermsg_mutex);
	if(source->is_srtp) {
		g_free(source->srtpcrypto);
		srtp_dealloc(source->srtp_ctx);
		g_free(source->srtp_policy.key);
	}
#ifdef HAVE_LIBCURL
	janus_mutex_lock(&source->rtsp_mutex);
	if(source->curl) {
		/* Send an RTSP TEARDOWN */
		curl_easy_setopt(source->curl, CURLOPT_RTSP_REQUEST, (long)CURL_RTSPREQ_TEARDOWN);
		int res = curl_easy_perform(source->curl);
		if(res != CURLE_OK) {
			JANUS_LOG(LOG_ERR, "Couldn't send TEARDOWN request: %s\n", curl_easy_strerror(res));
		}
		curl_easy_cleanup(source->curl);
	}
	janus_streaming_buffer *curldata = source->curldata;
	if(curldata != NULL) {
		g_free(curldata->buffer);
		g_free(curldata);
	}
	g_free(source->rtsp_url);
	g_free(source->rtsp_username);
	g_free(source->rtsp_password);
	g_free(source->rtsp_ahost);
	g_free(source->rtsp_vhost);
	janus_mutex_unlock(&source->rtsp_mutex);
#endif
	g_free(source);
}

static void janus_streaming_file_source_free(janus_streaming_file_source *source) {
	g_free(source->filename);
	g_free(source);
}

/* Helper to create an RTP live source (e.g., from gstreamer/ffmpeg/vlc/etc.) */
janus_streaming_mountpoint *janus_streaming_create_rtp_source(
		uint64_t id, char *id_str, char *name, char *desc, char *metadata,
		int srtpsuite, char *srtpcrypto, int threads, gboolean e2ee,
		gboolean doaudio, gboolean doaudiortcp, char *amcast, const janus_network_address *aiface, uint16_t aport, uint16_t artcpport, uint8_t acodec, char *artpmap, char *afmtp, gboolean doaskew,
		gboolean dovideo, gboolean dovideortcp, char *vmcast, const janus_network_address *viface, uint16_t vport, uint16_t vrtcpport, uint8_t vcodec, char *vrtpmap, char *vfmtp, gboolean bufferkf,
			gboolean simulcast, uint16_t vport2, uint16_t vport3, gboolean svc, gboolean dovskew, int rtp_collision,
		gboolean dodata, const janus_network_address *diface, uint16_t dport, gboolean textdata, gboolean buffermsg) {
	char id_num[30];
	if(!string_ids) {
		g_snprintf(id_num, sizeof(id_num), "%"SCNu64, id);
		id_str = id_num;
	}
	char tempname[255];
	if(name == NULL) {
		JANUS_LOG(LOG_VERB, "Missing name, will generate a random one...\n");
		memset(tempname, 0, 255);
		g_snprintf(tempname, 255, "mp-%s", id_str);
	} else if(atoi(name) != 0) {
		JANUS_LOG(LOG_VERB, "Names can't start with a number, prefixing it...\n");
		memset(tempname, 0, 255);
		g_snprintf(tempname, 255, "mp-%s", name);
		name = NULL;
	}
	if(!doaudio && !dovideo && !dodata) {
		JANUS_LOG(LOG_ERR, "Can't add 'rtp' stream, no audio, video or data have to be streamed...\n");
		janus_mutex_lock(&mountpoints_mutex);
		g_hash_table_remove(mountpoints_temp, &id);
		janus_mutex_unlock(&mountpoints_mutex);
		return NULL;
	}
	if(doaudio && (artpmap == NULL)) {
		JANUS_LOG(LOG_ERR, "Can't add 'rtp' stream, missing mandatory information for audio...\n");
		janus_mutex_lock(&mountpoints_mutex);
		g_hash_table_remove(mountpoints_temp, &id);
		janus_mutex_unlock(&mountpoints_mutex);
		return NULL;
	}
	if(dovideo && (vcodec == 0 || vrtpmap == NULL)) {
		JANUS_LOG(LOG_ERR, "Can't add 'rtp' stream, missing mandatory information for video...\n");
		janus_mutex_lock(&mountpoints_mutex);
		g_hash_table_remove(mountpoints_temp, &id);
		janus_mutex_unlock(&mountpoints_mutex);
		return NULL;
	}
	JANUS_LOG(LOG_VERB, "Audio %s, Video %s, Data %s\n",
		doaudio ? "enabled" : "NOT enabled",
		dovideo ? "enabled" : "NOT enabled",
		dodata ? "enabled" : "NOT enabled");
	/* First of all, let's check if the requested ports are free */
	int audio_fd = -1;
	int audio_rtcp_fd = -1;
	char audiohost[46];
	audiohost[0] = '\0';
	if(doaudio) {
		audio_fd = janus_streaming_create_fd(aport, amcast ? inet_addr(amcast) : INADDR_ANY, aiface,
			audiohost, sizeof(audiohost), "Audio", "audio", name ? name : tempname, aport == 0);
		if(audio_fd < 0) {
			JANUS_LOG(LOG_ERR, "Can't bind to port %d for audio...\n", aport);
			janus_mutex_lock(&mountpoints_mutex);
			g_hash_table_remove(mountpoints_temp, &id);
			janus_mutex_unlock(&mountpoints_mutex);
			return NULL;
		}
		aport = janus_streaming_get_fd_port(audio_fd);
		if(doaudiortcp) {
			audio_rtcp_fd = janus_streaming_create_fd(artcpport, amcast ? inet_addr(amcast) : INADDR_ANY, aiface,
				NULL, 0, "Audio", "audio", name ? name : tempname, artcpport == 0);
			if(audio_rtcp_fd < 0) {
				JANUS_LOG(LOG_ERR, "Can't bind to port %d for audio RTCP...\n", artcpport);
				if(audio_fd > -1)
					close(audio_fd);
				janus_mutex_lock(&mountpoints_mutex);
				g_hash_table_remove(mountpoints_temp, &id);
				janus_mutex_unlock(&mountpoints_mutex);
				return NULL;
			}
			artcpport = janus_streaming_get_fd_port(audio_rtcp_fd);
		}
	}
	int video_fd[3] = {-1, -1, -1};
	int video_rtcp_fd = -1;
	char videohost[46];
	videohost[0] = '\0';
	if(dovideo) {
		video_fd[0] = janus_streaming_create_fd(vport, vmcast ? inet_addr(vmcast) : INADDR_ANY, viface,
			videohost, sizeof(videohost), "Video", "video", name ? name : tempname, vport == 0);
		if(video_fd[0] < 0) {
			JANUS_LOG(LOG_ERR, "Can't bind to port %d for video...\n", vport);
			if(audio_fd > -1)
				close(audio_fd);
			if(audio_rtcp_fd > -1)
				close(audio_rtcp_fd);
			janus_mutex_lock(&mountpoints_mutex);
			g_hash_table_remove(mountpoints_temp, &id);
			janus_mutex_unlock(&mountpoints_mutex);
			return NULL;
		}
		vport = janus_streaming_get_fd_port(video_fd[0]);
		if(dovideortcp) {
			video_rtcp_fd = janus_streaming_create_fd(vrtcpport, vmcast ? inet_addr(vmcast) : INADDR_ANY, viface,
				NULL, 0, "Video", "video", name ? name : tempname, vrtcpport == 0);
			if(video_rtcp_fd < 0) {
				JANUS_LOG(LOG_ERR, "Can't bind to port %d for video RTCP...\n", vrtcpport);
				if(audio_fd > -1)
					close(audio_fd);
				if(audio_rtcp_fd > -1)
					close(audio_rtcp_fd);
				if(video_fd[0] > -1)
					close(video_fd[0]);
				janus_mutex_lock(&mountpoints_mutex);
				g_hash_table_remove(mountpoints_temp, &id);
				janus_mutex_unlock(&mountpoints_mutex);
				return NULL;
			}
			vrtcpport = janus_streaming_get_fd_port(video_rtcp_fd);
		}
		if(simulcast) {
			video_fd[1] = janus_streaming_create_fd(vport2, vmcast ? inet_addr(vmcast) : INADDR_ANY, viface,
				NULL, 0, "Video", "video", name ? name : tempname, FALSE);
			if(video_fd[1] < 0) {
				JANUS_LOG(LOG_ERR, "Can't bind to port %d for video (2nd port)...\n", vport2);
				if(audio_fd > -1)
					close(audio_fd);
				if(audio_rtcp_fd > -1)
					close(audio_rtcp_fd);
				if(video_fd[0] > -1)
					close(video_fd[0]);
				if(video_rtcp_fd > -1)
					close(video_rtcp_fd);
				janus_mutex_lock(&mountpoints_mutex);
				g_hash_table_remove(mountpoints_temp, &id);
				janus_mutex_unlock(&mountpoints_mutex);
				return NULL;
			}
			vport2 = janus_streaming_get_fd_port(video_fd[1]);
			video_fd[2] = janus_streaming_create_fd(vport3, vmcast ? inet_addr(vmcast) : INADDR_ANY, viface,
				NULL, 0, "Video", "video", name ? name : tempname, FALSE);
			if(video_fd[2] < 0) {
				JANUS_LOG(LOG_ERR, "Can't bind to port %d for video (3rd port)...\n", vport3);
				if(audio_fd > -1)
					close(audio_fd);
				if(audio_rtcp_fd > -1)
					close(audio_rtcp_fd);
				if(video_rtcp_fd > -1)
					close(video_rtcp_fd);
				if(video_fd[0] > -1)
					close(video_fd[0]);
				if(video_fd[1] > -1)
					close(video_fd[1]);
				janus_mutex_lock(&mountpoints_mutex);
				g_hash_table_remove(mountpoints_temp, &id);
				janus_mutex_unlock(&mountpoints_mutex);
				return NULL;
			}
			vport3 = janus_streaming_get_fd_port(video_fd[2]);
		}
	}
	int data_fd = -1;
	char datahost[46];
	datahost[0] = '\0';
	if(dodata) {
#ifdef HAVE_SCTP
		data_fd = janus_streaming_create_fd(dport, INADDR_ANY, diface,
			datahost, sizeof(datahost), "Data", "data", name ? name : tempname, FALSE);
		if(data_fd < 0) {
			JANUS_LOG(LOG_ERR, "Can't bind to port %d for data...\n", dport);
			if(audio_fd > -1)
				close(audio_fd);
			if(audio_rtcp_fd > -1)
				close(audio_rtcp_fd);
			if(video_rtcp_fd > -1)
				close(video_rtcp_fd);
			if(video_fd[0] > -1)
				close(video_fd[0]);
			if(video_fd[1] > -1)
				close(video_fd[1]);
			if(video_fd[2] > -1)
				close(video_fd[2]);
			janus_mutex_lock(&mountpoints_mutex);
			g_hash_table_remove(mountpoints_temp, &id);
			janus_mutex_unlock(&mountpoints_mutex);
			return NULL;
		}
		dport = janus_streaming_get_fd_port(data_fd);
#else
		JANUS_LOG(LOG_WARN, "Mountpoint wants to do datachannel relaying, but datachannels support was not compiled...\n");
		dodata = FALSE;
#endif
	}
	/* Create the mountpoint */
	janus_network_address nil;
	janus_network_address_nullify(&nil);

	janus_streaming_mountpoint *live_rtp = g_malloc0(sizeof(janus_streaming_mountpoint));
	live_rtp->id = id;
	live_rtp->id_str = g_strdup(id_str);
	live_rtp->name = g_strdup(name ? name : tempname);
	char *description = NULL;
	if(desc != NULL)
		description = g_strdup(desc);
	else
		description = g_strdup(name ? name : tempname);
	live_rtp->description = description;
	live_rtp->metadata = (metadata ? g_strdup(metadata) : NULL);
	live_rtp->enabled = TRUE;
	live_rtp->active = FALSE;
	live_rtp->audio = doaudio;
	live_rtp->video = dovideo;
	live_rtp->data = dodata;
	live_rtp->streaming_type = janus_streaming_type_live;
	live_rtp->streaming_source = janus_streaming_source_rtp;
	janus_streaming_rtp_source *live_rtp_source = g_malloc0(sizeof(janus_streaming_rtp_source));
	/* First of all, let's check if we need to setup an SRTP mountpoint */
	if(srtpsuite > 0 && srtpcrypto != NULL) {
		/* Base64 decode the crypto string and set it as the SRTP context */
		gsize len = 0;
		guchar *decoded = g_base64_decode(srtpcrypto, &len);
		if(len < SRTP_MASTER_LENGTH) {
			JANUS_LOG(LOG_ERR, "Invalid SRTP crypto (%s)\n", srtpcrypto);
			g_free(decoded);
			if(audio_fd > -1)
				close(audio_fd);
			if(audio_rtcp_fd > -1)
				close(audio_rtcp_fd);
			if(video_rtcp_fd > -1)
				close(video_rtcp_fd);
			if(video_fd[0] > -1)
				close(video_fd[0]);
			if(video_fd[1] > -1)
				close(video_fd[1]);
			if(video_fd[2] > -1)
				close(video_fd[2]);
			if(data_fd > -1)
				close(data_fd);
			janus_mutex_lock(&mountpoints_mutex);
			g_hash_table_remove(mountpoints_temp, &id);
			janus_mutex_unlock(&mountpoints_mutex);
			g_free(live_rtp_source);
			g_free(live_rtp->name);
			g_free(live_rtp->description);
			g_free(live_rtp->metadata);
			g_free(live_rtp);
			return NULL;
		}
		/* Set SRTP policy */
		srtp_policy_t *policy = &live_rtp_source->srtp_policy;
		srtp_crypto_policy_set_rtp_default(&(policy->rtp));
		if(srtpsuite == 32) {
			srtp_crypto_policy_set_aes_cm_128_hmac_sha1_32(&(policy->rtp));
		} else if(srtpsuite == 80) {
			srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&(policy->rtp));
		}
		policy->ssrc.type = ssrc_any_inbound;
		policy->key = decoded;
		policy->next = NULL;
		/* Create SRTP context */
		srtp_err_status_t res = srtp_create(&live_rtp_source->srtp_ctx, policy);
		if(res != srtp_err_status_ok) {
			/* Something went wrong... */
			JANUS_LOG(LOG_ERR, "Error creating forwarder SRTP session: %d (%s)\n", res, janus_srtp_error_str(res));
			g_free(decoded);
			if(audio_fd > -1)
				close(audio_fd);
			if(audio_rtcp_fd > -1)
				close(audio_rtcp_fd);
			if(video_rtcp_fd > -1)
				close(video_rtcp_fd);
			if(video_fd[0] > -1)
				close(video_fd[0]);
			if(video_fd[1] > -1)
				close(video_fd[1]);
			if(video_fd[2] > -1)
				close(video_fd[2]);
			if(data_fd > -1)
				close(data_fd);
			janus_mutex_lock(&mountpoints_mutex);
			g_hash_table_remove(mountpoints_temp, &id);
			janus_mutex_unlock(&mountpoints_mutex);
			g_free(live_rtp_source);
			g_free(live_rtp->name);
			g_free(live_rtp->description);
			g_free(live_rtp->metadata);
			g_free(live_rtp);
			return NULL;
		}
		live_rtp_source->is_srtp = TRUE;
		live_rtp_source->srtpsuite = srtpsuite;
		live_rtp_source->srtpcrypto = g_strdup(srtpcrypto);
	}
	live_rtp_source->e2ee = e2ee;
	live_rtp_source->audio_mcast = doaudio ? (amcast ? inet_addr(amcast) : INADDR_ANY) : INADDR_ANY;
	live_rtp_source->audio_iface = doaudio && !janus_network_address_is_null(aiface) ? *aiface : nil;
	live_rtp_source->audio_port = doaudio ? aport : -1;
	live_rtp_source->audio_rtcp_port = artcpport;
	live_rtp_source->askew = doaskew;
	if(doaudio && strlen(audiohost) > 0)
		live_rtp_source->audio_host = g_strdup(audiohost);
	live_rtp_source->video_mcast = dovideo ? (vmcast ? inet_addr(vmcast) : INADDR_ANY) : INADDR_ANY;
	live_rtp_source->video_port[0] = dovideo ? vport : -1;
	live_rtp_source->video_rtcp_port = vrtcpport;
	live_rtp_source->simulcast = dovideo && simulcast;
	live_rtp_source->video_port[1] = live_rtp_source->simulcast ? vport2 : -1;
	live_rtp_source->video_port[2] = live_rtp_source->simulcast ? vport3 : -1;
	live_rtp_source->video_iface = dovideo && !janus_network_address_is_null(viface) ? *viface : nil;
	live_rtp_source->vskew = dovskew;
	if(dovideo && strlen(videohost) > 0)
		live_rtp_source->video_host = g_strdup(videohost);
	live_rtp_source->data_port = dodata ? dport : -1;
	live_rtp_source->data_iface = dodata && !janus_network_address_is_null(diface) ? *diface : nil;
	if(dodata && strlen(datahost) > 0)
		live_rtp_source->data_host = g_strdup(datahost);
	live_rtp_source->arc = NULL;
	live_rtp_source->vrc = NULL;
	live_rtp_source->drc = NULL;
	janus_rtp_switching_context_reset(&live_rtp_source->context[0]);
	janus_rtp_switching_context_reset(&live_rtp_source->context[1]);
	janus_rtp_switching_context_reset(&live_rtp_source->context[2]);
	janus_mutex_init(&live_rtp_source->rec_mutex);
	live_rtp_source->audio_fd = audio_fd;
	live_rtp_source->audio_rtcp_fd = audio_rtcp_fd;
	live_rtp_source->video_rtcp_fd = video_rtcp_fd;
	live_rtp_source->video_fd[0] = video_fd[0];
	live_rtp_source->video_fd[1] = video_fd[1];
	live_rtp_source->video_fd[2] = video_fd[2];
	live_rtp_source->data_fd = data_fd;
	live_rtp_source->pipefd[0] = -1;
	live_rtp_source->pipefd[1] = -1;
	pipe(live_rtp_source->pipefd);
	live_rtp_source->last_received_audio = janus_get_monotonic_time();
	live_rtp_source->last_received_video = janus_get_monotonic_time();
	live_rtp_source->last_received_data = janus_get_monotonic_time();
	live_rtp_source->keyframe.enabled = bufferkf;
	live_rtp_source->keyframe.latest_keyframe = NULL;
	live_rtp_source->keyframe.temp_keyframe = NULL;
	live_rtp_source->keyframe.temp_ts = 0;
	janus_mutex_init(&live_rtp_source->keyframe.mutex);
	live_rtp_source->rtp_collision = rtp_collision;
	live_rtp_source->textdata = textdata;
	live_rtp_source->buffermsg = buffermsg;
	live_rtp_source->last_msg = NULL;
	janus_mutex_init(&live_rtp_source->buffermsg_mutex);
	live_rtp->source = live_rtp_source;
	live_rtp->source_destroy = (GDestroyNotify) janus_streaming_rtp_source_free;
	live_rtp->codecs.audio_pt = doaudio ? acodec : -1;
	live_rtp->codecs.audio_rtpmap = doaudio ? g_strdup(artpmap) : NULL;
	live_rtp->codecs.audio_fmtp = doaudio ? (afmtp ? g_strdup(afmtp) : NULL) : NULL;
	live_rtp->codecs.video_codec = JANUS_VIDEOCODEC_NONE;
	if(dovideo) {
		if(strstr(vrtpmap, "vp8") || strstr(vrtpmap, "VP8"))
			live_rtp->codecs.video_codec = JANUS_VIDEOCODEC_VP8;
		else if(strstr(vrtpmap, "vp9") || strstr(vrtpmap, "VP9"))
			live_rtp->codecs.video_codec = JANUS_VIDEOCODEC_VP9;
		else if(strstr(vrtpmap, "h264") || strstr(vrtpmap, "H264"))
			live_rtp->codecs.video_codec = JANUS_VIDEOCODEC_H264;
		else if(strstr(vrtpmap, "av1") || strstr(vrtpmap, "AV1"))
			live_rtp->codecs.video_codec = JANUS_VIDEOCODEC_AV1;
		else if(strstr(vrtpmap, "h265") || strstr(vrtpmap, "H265"))
			live_rtp->codecs.video_codec = JANUS_VIDEOCODEC_H265;
	}
	if(svc) {
		if(live_rtp->codecs.video_codec == JANUS_VIDEOCODEC_VP9) {
			live_rtp_source->svc = TRUE;
		} else {
			JANUS_LOG(LOG_WARN, "SVC is only supported, in an experimental way, for VP9-SVC mountpoints: disabling it...\n");
		}
	}
	live_rtp->codecs.video_pt = dovideo ? vcodec : -1;
	live_rtp->codecs.video_rtpmap = dovideo ? g_strdup(vrtpmap) : NULL;
	live_rtp->codecs.video_fmtp = dovideo ? (vfmtp ? g_strdup(vfmtp) : NULL) : NULL;
	live_rtp->viewers = NULL;
	g_atomic_int_set(&live_rtp->destroyed, 0);
	janus_refcount_init(&live_rtp->ref, janus_streaming_mountpoint_free);
	janus_mutex_init(&live_rtp->mutex);
	janus_mutex_lock(&mountpoints_mutex);
	g_hash_table_insert(mountpoints,
		string_ids ? (gpointer)g_strdup(live_rtp->id_str) : (gpointer)janus_uint64_dup(live_rtp->id),
		live_rtp);
	g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)live_rtp->id_str : (gpointer)&live_rtp->id);
	janus_mutex_unlock(&mountpoints_mutex);
	/* If we need helper threads, spawn them now */
	GError *error = NULL;
	char tname[16];
	if(threads > 0) {
		int i=0;
		for(i=0; i<threads; i++) {
			janus_streaming_helper *helper = g_malloc0(sizeof(janus_streaming_helper));
			helper->id = i+1;
			helper->mp = live_rtp;
			helper->queued_packets = g_async_queue_new_full((GDestroyNotify)janus_streaming_rtp_relay_packet_free);
			janus_mutex_init(&helper->mutex);
			janus_refcount_init(&helper->ref, janus_streaming_helper_free);
			live_rtp->helper_threads++;
			/* Spawn a thread and add references */
			g_snprintf(tname, sizeof(tname), "help %u-%"SCNu64, helper->id, live_rtp->id);
			janus_refcount_increase(&live_rtp->ref);
			janus_refcount_increase(&helper->ref);
			helper->thread = g_thread_try_new(tname, &janus_streaming_helper_thread, helper, &error);
			if(error != NULL) {
				JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the helper thread...\n",
					error->code, error->message ? error->message : "??");
				g_error_free(error);
				janus_refcount_decrease(&live_rtp->ref);	/* This is for the helper thread */
				g_async_queue_unref(helper->queued_packets);
				janus_refcount_decrease(&helper->ref);
				/* This extra unref is for the init */
				janus_refcount_decrease(&helper->ref);
				janus_streaming_mountpoint_destroy(live_rtp);
				g_free(helper);
				return NULL;
			}
			janus_refcount_increase(&helper->ref);
			live_rtp->threads = g_list_append(live_rtp->threads, helper);
		}
	}
	/* Finally, create the mountpoint thread itself */
	g_snprintf(tname, sizeof(tname), "mp %s", live_rtp->id_str);
	janus_refcount_increase(&live_rtp->ref);
	live_rtp->thread = g_thread_try_new(tname, &janus_streaming_relay_thread, live_rtp, &error);
	if(error != NULL) {
		JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the RTP thread...\n",
			error->code, error->message ? error->message : "??");
		g_error_free(error);
		janus_refcount_decrease(&live_rtp->ref);	/* This is for the failed thread */
		janus_streaming_mountpoint_destroy(live_rtp);
		return NULL;
	}
	return live_rtp;
}

/* Helper to create a file/ondemand live source */
janus_streaming_mountpoint *janus_streaming_create_file_source(
		uint64_t id, char *id_str, char *name, char *desc, char *metadata, char *filename, gboolean live,
		gboolean doaudio, uint8_t acodec, char *artpmap, char *afmtp, gboolean dovideo) {
	char id_num[30];
	if(!string_ids) {
		g_snprintf(id_num, sizeof(id_num), "%"SCNu64, id);
		id_str = id_num;
	}
	if(filename == NULL) {
		JANUS_LOG(LOG_ERR, "Can't add 'live' stream, missing filename...\n");
		return NULL;
	}
	if(name == NULL) {
		JANUS_LOG(LOG_VERB, "Missing name, will generate a random one...\n");
	}
	if(!doaudio && !dovideo) {
		JANUS_LOG(LOG_ERR, "Can't add 'file' stream, no audio or video have to be streamed...\n");
		janus_mutex_lock(&mountpoints_mutex);
		g_hash_table_remove(mountpoints_temp, &id);
		janus_mutex_unlock(&mountpoints_mutex);
		return NULL;
	}
	/* FIXME We don't support video streaming from file yet */
	if(!doaudio || dovideo) {
		JANUS_LOG(LOG_ERR, "Can't add 'file' stream, we only support audio file streaming right now...\n");
		janus_mutex_lock(&mountpoints_mutex);
		g_hash_table_remove(mountpoints_temp, &id);
		janus_mutex_unlock(&mountpoints_mutex);
		return NULL;
	}
	/* TODO We should support something more than raw a-Law and mu-Law streams... */
#ifdef HAVE_LIBOGG
	if(!strstr(filename, ".opus") && !strstr(filename, ".alaw") && !strstr(filename, ".mulaw")) {
		JANUS_LOG(LOG_ERR, "Can't add 'file' stream, unsupported format (we only support Opus and raw mu-Law/a-Law files right now)\n");
#else
	if(!strstr(filename, ".alaw") && !strstr(filename, ".mulaw")) {
		JANUS_LOG(LOG_ERR, "Can't add 'file' stream, unsupported format (we only support raw mu-Law and a-Law files right now)\n");
#endif
		janus_mutex_lock(&mountpoints_mutex);
		g_hash_table_remove(mountpoints_temp, &id);
		janus_mutex_unlock(&mountpoints_mutex);
		return NULL;
	}
#ifdef HAVE_LIBOGG
	if(strstr(filename, ".opus") && (artpmap == NULL || strstr(artpmap, "opus/48000") == NULL)) {
		JANUS_LOG(LOG_ERR, "Can't add 'file' stream, opus file is not associated with an opus rtpmap\n");
		janus_mutex_lock(&mountpoints_mutex);
		g_hash_table_remove(mountpoints_temp, &id);
		janus_mutex_unlock(&mountpoints_mutex);
		return NULL;
	}
#endif
	janus_streaming_mountpoint *file_source = g_malloc0(sizeof(janus_streaming_mountpoint));
	file_source->id = id;
	file_source->id_str = g_strdup(id_str);
	char tempname[255];
	if(!name) {
		memset(tempname, 0, 255);
		g_snprintf(tempname, 255, "mp-%s", file_source->id_str);
	} else if(atoi(name) != 0) {
		memset(tempname, 0, 255);
		g_snprintf(tempname, 255, "mp-%s", name);
		name = NULL;
	}
	file_source->name = g_strdup(name ? name : tempname);
	char *description = NULL;
	if(desc != NULL)
		description = g_strdup(desc);
	else
		description = g_strdup(name ? name : tempname);
	file_source->description = description;
	file_source->metadata = (metadata ? g_strdup(metadata) : NULL);
	file_source->enabled = TRUE;
	file_source->active = FALSE;
	file_source->audio = TRUE;
	file_source->video = FALSE;
	file_source->data = FALSE;
	file_source->streaming_type = live ? janus_streaming_type_live : janus_streaming_type_on_demand;
	file_source->streaming_source = janus_streaming_source_file;
	janus_streaming_file_source *file_source_source = g_malloc0(sizeof(janus_streaming_file_source));
	file_source_source->filename = g_strdup(filename);
	file_source->source = file_source_source;
	file_source->source_destroy = (GDestroyNotify) janus_streaming_file_source_free;
	if(strstr(filename, ".opus")) {
		file_source_source->opus = TRUE;
		file_source->codecs.audio_pt = acodec;
		file_source->codecs.audio_rtpmap = g_strdup(artpmap);
		file_source->codecs.audio_fmtp = afmtp ? g_strdup(afmtp) : NULL;
	} else {
		file_source->codecs.audio_pt = strstr(filename, ".alaw") ? 8 : 0;
		file_source->codecs.audio_rtpmap = g_strdup(strstr(filename, ".alaw") ? "PCMA/8000" : "PCMU/8000");
	}
	file_source->codecs.video_pt = -1;	/* FIXME We don't support video for this type yet */
	file_source->codecs.video_rtpmap = NULL;
	file_source->viewers = NULL;
	g_atomic_int_set(&file_source->destroyed, 0);
	janus_refcount_init(&file_source->ref, janus_streaming_mountpoint_free);
	janus_mutex_init(&file_source->mutex);
	janus_mutex_lock(&mountpoints_mutex);
	g_hash_table_insert(mountpoints,
		string_ids ? (gpointer)g_strdup(file_source->id_str) : (gpointer)janus_uint64_dup(file_source->id),
		file_source);
	g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)file_source->id_str : (gpointer)&file_source->id);
	janus_mutex_unlock(&mountpoints_mutex);
	if(live) {
		GError *error = NULL;
		char tname[16];
		g_snprintf(tname, sizeof(tname), "mp %s", file_source->id_str);
		janus_refcount_increase(&file_source->ref);
		file_source->thread = g_thread_try_new(tname, &janus_streaming_filesource_thread, file_source, &error);
		if(error != NULL) {
			JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the live filesource thread...\n",
				error->code, error->message ? error->message : "??");
			g_error_free(error);
			janus_refcount_decrease(&file_source->ref);		/* This is for the failed thread */
			janus_refcount_decrease(&file_source->ref);
			return NULL;
		}
	}
	return file_source;
}

#ifdef HAVE_LIBCURL
static size_t janus_streaming_rtsp_curl_callback(void *payload, size_t size, size_t nmemb, void *data) {
	size_t realsize = size * nmemb;
	janus_streaming_buffer *buf = (struct janus_streaming_buffer *)data;
	/* (Re)allocate if needed */
	buf->buffer = realloc(buf->buffer, buf->size+realsize+1);
	/* Update the buffer */
	memcpy(&(buf->buffer[buf->size]), payload, realsize);
	buf->size += realsize;
	buf->buffer[buf->size] = 0;
	/* Done! */
	return realsize;
}

static int janus_streaming_rtsp_parse_sdp(const char *buffer, const char *name, const char *media, char *base, int *pt,
		char *transport, char *host, char *rtpmap, char *fmtp, char *control, const janus_network_address *iface, multiple_fds *fds) {
	/* Start by checking if there's any Content-Base header we should be aware of */
	const char *cb = strstr(buffer, "Content-Base:");
	if(cb == NULL)
		cb = strstr(buffer, "content-base:");
	if(cb != NULL) {
		cb = strstr(cb, "rtsp://");
		const char *crlf = (cb ? strstr(cb, "\r\n") : NULL);
		if(crlf != NULL && base != NULL) {
			gulong size = (crlf-cb)+1;
			if(size > 256)
				size = 256;
			g_snprintf(base, size, "%s", cb);
			if(base[size-2] == '/')
				base[size-2] = '\0';
		}
	}
	/* Parse the SDP now */
	char pattern[256];
	g_snprintf(pattern, sizeof(pattern), "m=%s", media);
	char *m = strstr(buffer, pattern);
	if(m == NULL) {
		JANUS_LOG(LOG_VERB, "[%s] no media %s...\n", name, media);
		return -1;
	}
	sscanf(m, "m=%*s %*d %*s %d", pt);
	char *s = strstr(m, "a=control:");
	if(s == NULL) {
		JANUS_LOG(LOG_ERR, "[%s] no control for %s...\n", name, media);
		return -1;
	}
	sscanf(s, "a=control:%2047s", control);
	char *r = strstr(m, "a=rtpmap:");
	if(r != NULL) {
		if (sscanf(r, "a=rtpmap:%*d%*[ ]%2047[^\r\n]s", rtpmap) != 1) {
			JANUS_LOG(LOG_ERR, "[%s] cannot parse %s rtpmap...\n", name, media);
			return -1;
		}
	}
	char *f = strstr(m, "a=fmtp:");
	if(f != NULL) {
		if (sscanf(f, "a=fmtp:%*d%*[ ]%2047[^\r\n]s", fmtp) != 1) {
			JANUS_LOG(LOG_ERR, "[%s] cannot parse %s fmtp...\n", name, media);
			return -1;
		}
	}
	char *c = strstr(m, "c=IN IP4");
	if(c == NULL) {
		/* No m-line c= attribute? try in the whole SDP */
		c = strstr(buffer, "c=IN IP4");
	}
	char ip[256];
	in_addr_t mcast = INADDR_ANY;
	if(c != NULL) {
		if(sscanf(c, "c=IN IP4 %255[^/]", ip) != 0) {
			memcpy(host, ip, sizeof(ip));
			c = strstr(host, "\r\n");
			if(c)
				*c = '\0';
			mcast = inet_addr(ip);
		}
	}
	/* Bind two adjacent ports for RTP and RTCP */
	int ports[2];
	if(janus_streaming_allocate_port_pair(name, media, mcast, iface, fds, ports)) {
		JANUS_LOG(LOG_ERR, "[%s] Bind failed for %s...\n", name, media);
		return -1;
	}

	if(IN_MULTICAST(ntohl(mcast))) {
		g_snprintf(transport, 1024, "RTP/AVP/UDP;multicast;client_port=%d-%d", ports[0], ports[1]);
	} else {
		g_snprintf(transport, 1024, "RTP/AVP/UDP;unicast;client_port=%d-%d", ports[0], ports[1]);
	}

	return 0;
}

/* Static helper to connect to an RTSP server, considering we might do this either
 * when creating a new mountpoint, or when reconnecting after some failure */
static int janus_streaming_rtsp_connect_to_server(janus_streaming_mountpoint *mp) {
	if(mp == NULL)
		return -1;
	janus_streaming_rtp_source *source = (janus_streaming_rtp_source *)mp->source;
	if(source == NULL)
		return -1;

	char *name = mp->name;
	gboolean doaudio = mp->audio;
	gboolean dovideo = mp->video;

	CURL *curl = curl_easy_init();
	if(curl == NULL) {
		JANUS_LOG(LOG_ERR, "Can't init CURL\n");
		return -1;
	}
	if(janus_log_level > LOG_INFO)
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
	curl_easy_setopt(curl, CURLOPT_URL, source->rtsp_url);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 0L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	/* Any authentication to take into account? */
	if(source->rtsp_username && source->rtsp_password) {
		/* Point out that digest authentication is only available is libcurl >= 7.45.0 */
		if(LIBCURL_VERSION_NUM < 0x072d00) {
			JANUS_LOG(LOG_WARN, "RTSP digest authentication unsupported (needs libcurl >= 7.45.0)\n");
		}
		curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
		curl_easy_setopt(curl, CURLOPT_USERNAME, source->rtsp_username);
		curl_easy_setopt(curl, CURLOPT_PASSWORD, source->rtsp_password);
	}
	/* Send an RTSP DESCRIBE */
	janus_streaming_buffer *curldata = g_malloc(sizeof(janus_streaming_buffer));
	curldata->buffer = g_malloc0(1);
	curldata->size = 0;
	curl_easy_setopt(curl, CURLOPT_RTSP_STREAM_URI, source->rtsp_url);
	curl_easy_setopt(curl, CURLOPT_RTSP_REQUEST, (long)CURL_RTSPREQ_DESCRIBE);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, janus_streaming_rtsp_curl_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, curldata);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, janus_streaming_rtsp_curl_callback);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, curldata);
	int res = curl_easy_perform(curl);
	if(res != CURLE_OK) {
		JANUS_LOG(LOG_ERR, "Couldn't send DESCRIBE request: %s\n", curl_easy_strerror(res));
		curl_easy_cleanup(curl);
		g_free(curldata->buffer);
		g_free(curldata);
		return -2;
	}
	long code = 0;
	res = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	if(res != CURLE_OK) {
		JANUS_LOG(LOG_ERR, "Couldn't get DESCRIBE answer: %s\n", curl_easy_strerror(res));
		curl_easy_cleanup(curl);
		g_free(curldata->buffer);
		g_free(curldata);
		return -3;
	} else if(code != 200) {
		JANUS_LOG(LOG_ERR, "Couldn't get DESCRIBE code: %ld\n", code);
		curl_easy_cleanup(curl);
		g_free(curldata->buffer);
		g_free(curldata);
		return -4;
	}
	JANUS_LOG(LOG_VERB, "DESCRIBE answer:%s\n", curldata->buffer);
	/* Parse the SDP we just got to figure out the negotiated media */
	int ka_timeout = 0;
	int vpt = -1;
	char vrtpmap[2048];
	vrtpmap[0] = '\0';
	char vfmtp[2048];
	vfmtp[0] = '\0';
	char vcontrol[2048];
	char uri[1024];
	char vtransport[1024];
	char vhost[256];
	vhost[0] = '\0';
	char vbase[256];
	vbase[0] = '\0';
	int vsport = 0, vsport_rtcp = 0;
	multiple_fds video_fds = {-1, -1};

	int apt = -1;
	char artpmap[2048];
	artpmap[0] = '\0';
	char afmtp[2048];
	afmtp[0] = '\0';
	char acontrol[2048];
	char atransport[1024];
	char ahost[256];
	ahost[0] = '\0';
	char abase[256];
	abase[0] = '\0';
	int asport = 0, asport_rtcp = 0;
	multiple_fds audio_fds = {-1, -1};

	janus_mutex_lock(&mountpoints_mutex);
	/* Parse both video and audio first before proceed to setup as curldata will be reused */
	int vresult = -1;
	if(dovideo) {
		vresult = janus_streaming_rtsp_parse_sdp(curldata->buffer, name, "video", vbase, &vpt,
			vtransport, vhost, vrtpmap, vfmtp, vcontrol, &source->video_iface, &video_fds);
	}
	int aresult = -1;
	if(doaudio) {
		aresult = janus_streaming_rtsp_parse_sdp(curldata->buffer, name, "audio", abase, &apt,
			atransport, ahost, artpmap, afmtp, acontrol, &source->audio_iface, &audio_fds);
	}
	janus_mutex_unlock(&mountpoints_mutex);

	if(vresult == -1 && aresult == -1) {
		/* Both audio and video failed? Give up... */
		return -7;
	}

	/* Check if a query string is part of the URL, as that may impact the SETUP request */
	char *rtsp_url = source->rtsp_url, *rtsp_querystring = NULL;
	char **parts = g_strsplit(source->rtsp_url, "?", 2);
	if(parts[0] != NULL) {
		rtsp_url = parts[0];
		rtsp_querystring = parts[1];
	}

	if(vresult != -1) {
		/* Identify video codec (useful for keyframe detection) */
		mp->codecs.video_codec = JANUS_VIDEOCODEC_NONE;
		if(strstr(vrtpmap, "vp8") || strstr(vrtpmap, "VP8"))
			mp->codecs.video_codec = JANUS_VIDEOCODEC_VP8;
		else if(strstr(vrtpmap, "vp9") || strstr(vrtpmap, "VP9"))
			mp->codecs.video_codec = JANUS_VIDEOCODEC_VP9;
		else if(strstr(vrtpmap, "h264") || strstr(vrtpmap, "H264"))
			mp->codecs.video_codec = JANUS_VIDEOCODEC_H264;

		/* Send an RTSP SETUP for video */
		g_free(curldata->buffer);
		curldata->buffer = g_malloc0(1);
		curldata->size = 0;
		gboolean add_qs = (rtsp_querystring != NULL);
		if(add_qs && strstr(vcontrol, rtsp_querystring) != NULL)
			add_qs = FALSE;
		if(strstr(vcontrol, (strlen(vbase) > 0 ? vbase : rtsp_url)) == vcontrol) {
			/* The control attribute already contains the whole URL? */
			g_snprintf(uri, sizeof(uri), "%s%s%s", vcontrol,
				add_qs ? "?" : "", add_qs ? rtsp_querystring : "");
		} else {
			/* Append the control attribute to the URL */
			g_snprintf(uri, sizeof(uri), "%s/%s%s%s", (strlen(vbase) > 0 ? vbase : rtsp_url),
				vcontrol, add_qs ? "?" : "", add_qs ? rtsp_querystring : "");
		}
		curl_easy_setopt(curl, CURLOPT_RTSP_STREAM_URI, uri);
		curl_easy_setopt(curl, CURLOPT_RTSP_TRANSPORT, vtransport);
		curl_easy_setopt(curl, CURLOPT_RTSP_REQUEST, (long)CURL_RTSPREQ_SETUP);
		res = curl_easy_perform(curl);
		if(res != CURLE_OK) {
			JANUS_LOG(LOG_ERR, "Couldn't send SETUP request: %s\n", curl_easy_strerror(res));
			g_strfreev(parts);
			curl_easy_cleanup(curl);
			g_free(curldata->buffer);
			g_free(curldata);
			if(video_fds.fd != -1) close(video_fds.fd);
			if(video_fds.rtcp_fd != -1) close(video_fds.rtcp_fd);
			if(audio_fds.fd != -1) close(audio_fds.fd);
			if(audio_fds.rtcp_fd != -1) close(audio_fds.rtcp_fd);
			return -5;
		} else if(code != 200) {
			JANUS_LOG(LOG_ERR, "Couldn't get SETUP code: %ld\n", code);
			g_strfreev(parts);
			curl_easy_cleanup(curl);
			g_free(curldata->buffer);
			g_free(curldata);
			if(video_fds.fd != -1) close(video_fds.fd);
			if(video_fds.rtcp_fd != -1) close(video_fds.rtcp_fd);
			if(audio_fds.fd != -1) close(audio_fds.fd);
			if(audio_fds.rtcp_fd != -1) close(audio_fds.rtcp_fd);
			return -5;
		}
		JANUS_LOG(LOG_VERB, "SETUP answer:%s\n", curldata->buffer);
		/* Parse the RTSP message: we may need Transport and Session */
		gboolean success = TRUE;
		gchar **parts = g_strsplit(curldata->buffer, "\n", -1);
		if(parts) {
			int index = 0;
			char *line = NULL, *cr = NULL;
			while(success && (line = parts[index]) != NULL) {
				cr = strchr(line, '\r');
				if(cr != NULL)
					*cr = '\0';
				if(*line == '\0') {
					if(cr != NULL)
						*cr = '\r';
					index++;
					continue;
				}
				if(strlen(line) < 3) {
					JANUS_LOG(LOG_ERR, "Invalid RTSP line (%zu bytes): %s\n", strlen(line), line);
					success = FALSE;
					break;
				}
				/* Check if this is a Transport or Session header, and if so parse it */
				gboolean is_transport = (strstr(line, "Transport:") == line || strstr(line, "transport:") == line);
				gboolean is_session = (strstr(line, "Session:") == line || strstr(line, "session:") == line);
				if(is_transport || is_session) {
					/* There is, iterate on all params */
					char *p = line, param[100], *pi = NULL;
					int read = 0;
					gboolean first = TRUE;
					while(sscanf(p, "%99[^;]%n", param, &read) == 1) {
						if(first) {
							/* Skip */
							first = FALSE;
						} else {
							pi = param;
							while(*pi == ' ')
								pi++;
							char name[50], value[50];
							if(sscanf(pi, "%49[a-zA-Z_0-9]=%49s", name, value) == 2) {
								if(is_transport) {
									if(!strcasecmp(name, "ssrc")) {
										/* Take note of the video SSRC */
										uint32_t ssrc = strtol(value, NULL, 16);
										JANUS_LOG(LOG_VERB, "  -- SSRC (video): %"SCNu32"\n", ssrc);
										source->video_ssrc = ssrc;
									} else if(!strcasecmp(name, "source")) {
										/* If we got an address via c-line, replace it */
										g_snprintf(vhost, sizeof(vhost), "%s", value);
										JANUS_LOG(LOG_VERB, "  -- Source (video): %s\n", vhost);
									} else if(!strcasecmp(name, "server_port")) {
										/* Take note of the server port */
										char *dash = NULL;
										vsport = strtol(value, &dash, 10);
										vsport_rtcp = dash ? strtol(++dash, NULL, 10) : 0;
										JANUS_LOG(LOG_VERB, "  -- RTP port (video): %d\n", vsport);
										JANUS_LOG(LOG_VERB, "  -- RTCP port (video): %d\n", vsport_rtcp);
									}
								} else if(is_session) {
									if(!strcasecmp(name, "timeout")) {
										/* Take note of the timeout, for keep-alives */
										ka_timeout = atoi(value);
										JANUS_LOG(LOG_VERB, "  -- RTSP session timeout (video): %d\n", ka_timeout);
									}
								}
							}
						}
						/* Move to the next param */
						p += read;
						if(*p != ';')
							break;
						while(*p == ';')
							p++;
					}
				}
				if(cr != NULL)
					*cr = '\r';
				index++;
			}
			if(cr != NULL)
				*cr = '\r';
			g_strfreev(parts);
		}
#ifdef HAVE_LIBCURL
#if CURL_AT_LEAST_VERSION(7, 62, 0)
		/* If we don't have a host yet (no c-line, no source in Transport), use the server address */
		if(strlen(vhost) == 0 || !strcmp(vhost, "0.0.0.0")) {
			JANUS_LOG(LOG_WARN, "No c-line or source for RTSP video address, resolving server address...\n");
			CURLU *url = curl_url();
			if(url != NULL) {
				CURLUcode code = curl_url_set(url, CURLUPART_URL, source->rtsp_url, 0);
				if(code == 0) {
					char *host = NULL;
					code = curl_url_get(url, CURLUPART_HOST, &host, 0);
					if(code == 0) {
						/* Resolve the address */
						struct addrinfo *info = NULL, *start = NULL;
						janus_network_address addr;
						janus_network_address_string_buffer addr_buf;
						if(getaddrinfo(host, NULL, NULL, &info) == 0) {
							start = info;
							while(info != NULL) {
								if(janus_network_address_from_sockaddr(info->ai_addr, &addr) == 0 &&
										janus_network_address_to_string_buffer(&addr, &addr_buf) == 0) {
									/* Resolved */
									g_snprintf(vhost, sizeof(vhost), "%s",
										janus_network_address_string_from_buffer(&addr_buf));
									JANUS_LOG(LOG_VERB, "   -- %s\n", vhost);
									break;
								}
								info = info->ai_next;
							}
						}
						if(start)
							freeaddrinfo(start);
						curl_free(host);
					}
				}
				curl_url_cleanup(url);
			}
		}
#endif
#endif
		if(strlen(vhost) == 0 || !strcmp(vhost, "0.0.0.0")) {
			/* Still nothing... */
			JANUS_LOG(LOG_WARN, "No host address for the RTSP video stream, no latching will be performed\n");
		}
	}

	if(aresult != -1) {
		/* Send an RTSP SETUP for audio */
		g_free(curldata->buffer);
		curldata->buffer = g_malloc0(1);
		curldata->size = 0;
		gboolean add_qs = (rtsp_querystring != NULL);
		if(add_qs && strstr(acontrol, rtsp_querystring) != NULL)
			add_qs = FALSE;
		if(strstr(acontrol, (strlen(abase) > 0 ? abase : rtsp_url)) == acontrol) {
			/* The control attribute already contains the whole URL? */
			g_snprintf(uri, sizeof(uri), "%s%s%s", acontrol,
				add_qs ? "?" : "", add_qs ? rtsp_querystring : "");
		} else {
			/* Append the control attribute to the URL */
			g_snprintf(uri, sizeof(uri), "%s/%s%s%s", (strlen(abase) > 0 ? abase : rtsp_url),
				acontrol, add_qs ? "?" : "", add_qs ? rtsp_querystring : "");
		}
		curl_easy_setopt(curl, CURLOPT_RTSP_STREAM_URI, uri);
		curl_easy_setopt(curl, CURLOPT_RTSP_TRANSPORT, atransport);
		curl_easy_setopt(curl, CURLOPT_RTSP_REQUEST, (long)CURL_RTSPREQ_SETUP);
		res = curl_easy_perform(curl);
		if(res != CURLE_OK) {
			JANUS_LOG(LOG_ERR, "Couldn't send SETUP request: %s\n", curl_easy_strerror(res));
			g_strfreev(parts);
			curl_easy_cleanup(curl);
			g_free(curldata->buffer);
			g_free(curldata);
			if(video_fds.fd != -1) close(video_fds.fd);
			if(video_fds.rtcp_fd != -1) close(video_fds.rtcp_fd);
			if(audio_fds.fd != -1) close(audio_fds.fd);
			if(audio_fds.rtcp_fd != -1) close(audio_fds.rtcp_fd);
			return -6;
		} else if(code != 200) {
			JANUS_LOG(LOG_ERR, "Couldn't get SETUP code: %ld\n", code);
			g_strfreev(parts);
			curl_easy_cleanup(curl);
			g_free(curldata->buffer);
			g_free(curldata);
			if(video_fds.fd != -1) close(video_fds.fd);
			if(video_fds.rtcp_fd != -1) close(video_fds.rtcp_fd);
			if(audio_fds.fd != -1) close(audio_fds.fd);
			if(audio_fds.rtcp_fd != -1) close(audio_fds.rtcp_fd);
			return -6;
		}
		JANUS_LOG(LOG_VERB, "SETUP answer:%s\n", curldata->buffer);
		/* Parse the RTSP message: we may need Transport and Session */
		gboolean success = TRUE;
		gchar **parts = g_strsplit(curldata->buffer, "\n", -1);
		if(parts) {
			int index = 0;
			char *line = NULL, *cr = NULL;
			while(success && (line = parts[index]) != NULL) {
				cr = strchr(line, '\r');
				if(cr != NULL)
					*cr = '\0';
				if(*line == '\0') {
					if(cr != NULL)
						*cr = '\r';
					index++;
					continue;
				}
				if(strlen(line) < 3) {
					JANUS_LOG(LOG_ERR, "Invalid RTSP line (%zu bytes): %s\n", strlen(line), line);
					success = FALSE;
					break;
				}
				/* Check if this is a Transport or Session header, and if so parse it */
				gboolean is_transport = (strstr(line, "Transport:") == line || strstr(line, "transport:") == line);
				gboolean is_session = (strstr(line, "Session:") == line || strstr(line, "session:") == line);
				if(is_transport || is_session) {
					/* There is, iterate on all params */
					char *p = line, param[100], *pi = NULL;
					int read = 0;
					gboolean first = TRUE;
					while(sscanf(p, "%99[^;]%n", param, &read) == 1) {
						if(first) {
							/* Skip */
							first = FALSE;
						} else {
							pi = param;
							while(*pi == ' ')
								pi++;
							char name[50], value[50];
							if(sscanf(pi, "%49[a-zA-Z_0-9]=%49s", name, value) == 2) {
								if(is_transport) {
									if(!strcasecmp(name, "ssrc")) {
										/* Take note of the audio SSRC */
										uint32_t ssrc = strtol(value, NULL, 16);
										JANUS_LOG(LOG_VERB, "  -- SSRC (audio): %"SCNu32"\n", ssrc);
										source->audio_ssrc = ssrc;
									} else if(!strcasecmp(name, "source")) {
										/* If we got an address via c-line, replace it */
										g_snprintf(ahost, sizeof(ahost), "%s", value);
										JANUS_LOG(LOG_VERB, "  -- Source (audio): %s\n", ahost);
									} else if(!strcasecmp(name, "server_port")) {
										/* Take note of the server port */
										char *dash = NULL;
										asport = strtol(value, &dash, 10);
										asport_rtcp = dash ? strtol(++dash, NULL, 10) : 0;
										JANUS_LOG(LOG_VERB, "  -- RTP port (audio): %d\n", asport);
										JANUS_LOG(LOG_VERB, "  -- RTCP port (audio): %d\n", asport_rtcp);
									}
								} else if(is_session) {
									if(!strcasecmp(name, "timeout")) {
										/* Take note of the timeout, for keep-alives */
										ka_timeout = atoi(value);
										JANUS_LOG(LOG_VERB, "  -- RTSP session timeout (audio): %d\n", ka_timeout);
									}
								}
							}
						}
						/* Move to the next param */
						p += read;
						if(*p != ';')
							break;
						while(*p == ';')
							p++;
					}
				}
				if(cr != NULL)
					*cr = '\r';
				index++;
			}
			if(cr != NULL)
				*cr = '\r';
			g_strfreev(parts);
		}
		/* If we don't have a host yet (no c-line, no source in Transport), use the server address */
		if(strlen(ahost) == 0 || !strcmp(ahost, "0.0.0.0")) {
			if(strlen(vhost) > 0 && strcmp(vhost, "0.0.0.0")) {
				JANUS_LOG(LOG_WARN, "No c-line or source for RTSP audio stream, copying the video address (%s)\n", vhost);
				g_snprintf(ahost, sizeof(ahost), "%s", vhost);
			} else {
#ifdef HAVE_LIBCURL
#if CURL_AT_LEAST_VERSION(7, 62, 0)
				JANUS_LOG(LOG_WARN, "No c-line or source for RTSP audio stream, resolving server address...\n");
				CURLU *url = curl_url();
				if(url != NULL) {
					CURLUcode code = curl_url_set(url, CURLUPART_URL, source->rtsp_url, 0);
					if(code == 0) {
						char *host = NULL;
						code = curl_url_get(url, CURLUPART_HOST, &host, 0);
						if(code == 0) {
							/* Resolve the address */
							struct addrinfo *info = NULL, *start = NULL;
							janus_network_address addr;
							janus_network_address_string_buffer addr_buf;
							if(getaddrinfo(host, NULL, NULL, &info) == 0) {
								start = info;
								while(info != NULL) {
									if(janus_network_address_from_sockaddr(info->ai_addr, &addr) == 0 &&
											janus_network_address_to_string_buffer(&addr, &addr_buf) == 0) {
										/* Resolved */
										g_snprintf(ahost, sizeof(ahost), "%s",
											janus_network_address_string_from_buffer(&addr_buf));
										JANUS_LOG(LOG_VERB, "   -- %s\n", ahost);
										break;
									}
									info = info->ai_next;
								}
							}
							if(start)
								freeaddrinfo(start);
							curl_free(host);
						}
					}
					curl_url_cleanup(url);
				}
#endif
#endif
			}
		}
		if(strlen(ahost) == 0 || !strcmp(ahost, "0.0.0.0")) {
			/* Still nothing... */
			JANUS_LOG(LOG_WARN, "No host address for the RTSP audio stream, no latching will be performed\n");
		}
	}
	g_strfreev(parts);

	/* Update the source (but check if ptype/rtpmap/fmtp need to be overridden) */
	if(mp->codecs.audio_pt == -1)
		mp->codecs.audio_pt = doaudio ? apt : -1;
	if(mp->codecs.audio_rtpmap == NULL)
		mp->codecs.audio_rtpmap = (doaudio && strlen(artpmap)) ? g_strdup(artpmap) : NULL;
	if(mp->codecs.audio_fmtp == NULL)
		mp->codecs.audio_fmtp = (doaudio && strlen(afmtp)) ? g_strdup(afmtp) : NULL;
	if(mp->codecs.video_pt == -1)
		mp->codecs.video_pt = dovideo ? vpt : -1;
	if(mp->codecs.video_rtpmap == NULL)
		mp->codecs.video_rtpmap = (dovideo && strlen(vrtpmap)) ? g_strdup(vrtpmap) : NULL;
	if(mp->codecs.video_fmtp == NULL)
		mp->codecs.video_fmtp = (dovideo && strlen(vfmtp)) ? g_strdup(vfmtp) : NULL;
	source->audio_fd = audio_fds.fd;
	source->audio_rtcp_fd = audio_fds.rtcp_fd;
	source->remote_audio_port = asport;
	source->remote_audio_rtcp_port = asport_rtcp;
	g_free(source->rtsp_ahost);
	if(asport > 0)
		source->rtsp_ahost = g_strdup(ahost);
	source->video_fd[0] = video_fds.fd;
	source->video_rtcp_fd = video_fds.rtcp_fd;
	source->remote_video_port = vsport;
	source->remote_video_rtcp_port = vsport_rtcp;
	g_free(source->rtsp_vhost);
	if(vsport > 0)
		source->rtsp_vhost = g_strdup(vhost);
	source->curl = curl;
	source->curldata = curldata;
	source->ka_timeout = ka_timeout;
	return 0;
}

/* Helper method to send a latching packet on an RTSP media socket */
static void janus_streaming_rtsp_latch(int fd, char *host, int port, struct sockaddr *remote) {
	/* Resolve address to get an IP */
	struct addrinfo *res = NULL;
	janus_network_address addr;
	janus_network_address_string_buffer addr_buf;
	if(getaddrinfo(host, NULL, NULL, &res) != 0 ||
			janus_network_address_from_sockaddr(res->ai_addr, &addr) != 0 ||
			janus_network_address_to_string_buffer(&addr, &addr_buf) != 0) {
		JANUS_LOG(LOG_ERR, "Could not resolve %s...\n", host);
		if(res)
			freeaddrinfo(res);
	} else {
		freeaddrinfo(res);
		/* Prepare the recipient */
		struct sockaddr_in remote4 = { 0 };
		struct sockaddr_in6 remote6 = { 0 };
		socklen_t addrlen = 0;
		if(addr.family == AF_INET) {
			memset(&remote4, 0, sizeof(remote4));
			remote4.sin_family = AF_INET;
			remote4.sin_port = htons(port);
			memcpy(&remote4.sin_addr, &addr.ipv4, sizeof(addr.ipv4));
			remote = (struct sockaddr *)(&remote4);
			addrlen = sizeof(remote4);
		} else if(addr.family == AF_INET6) {
			memset(&remote6, 0, sizeof(remote6));
			remote6.sin6_family = AF_INET6;
			remote6.sin6_port = htons(port);
			memcpy(&remote6.sin6_addr, &addr.ipv6, sizeof(addr.ipv6));
			remote6.sin6_addr = addr.ipv6;
			remote = (struct sockaddr *)(&remote6);
			addrlen = sizeof(remote6);
		}
		/* Prepare an empty RTP packet */
		janus_rtp_header rtp;
		memset(&rtp, 0, sizeof(rtp));
		rtp.version = 2;
		/* Send a couple of latching packets */
		(void)sendto(fd, &rtp, 12, 0, remote, addrlen);
		(void)sendto(fd, &rtp, 12, 0, remote, addrlen);
	}
}

/* Helper to send an RTSP PLAY (either when we create the mountpoint, or when we try reconnecting) */
static int janus_streaming_rtsp_play(janus_streaming_rtp_source *source) {
	if(source == NULL || source->curldata == NULL)
		return -1;
	/* First of all, send a latching packet to the RTSP server port(s) */
	struct sockaddr_in6 remote = { 0 };
	if(source->remote_audio_port > 0 && source->audio_fd >= 0) {
		JANUS_LOG(LOG_VERB, "RTSP audio latching: %s:%d\n", source->rtsp_ahost, source->remote_audio_port);
		janus_streaming_rtsp_latch(source->audio_fd, source->rtsp_ahost,
			source->remote_audio_port, (struct sockaddr *)&remote);
		if(source->remote_audio_rtcp_port > 0 && source->audio_rtcp_fd >= 0) {
			JANUS_LOG(LOG_VERB, "  -- RTCP: %s:%d\n", source->rtsp_ahost, source->remote_audio_rtcp_port);
			janus_streaming_rtsp_latch(source->audio_rtcp_fd, source->rtsp_ahost,
				source->remote_audio_rtcp_port, (struct sockaddr *)&source->audio_rtcp_addr);
		}
	}
	if(source->remote_video_port > 0 && source->video_fd[0] >= 0) {
		JANUS_LOG(LOG_VERB, "RTSP video latching: %s:%d\n", source->rtsp_vhost, source->remote_video_port);
		janus_streaming_rtsp_latch(source->video_fd[0], source->rtsp_vhost,
			source->remote_video_port, (struct sockaddr *)&remote);
		if(source->remote_video_rtcp_port > 0 && source->video_rtcp_fd >= 0) {
			JANUS_LOG(LOG_VERB, "  -- RTCP: %s:%d\n", source->rtsp_vhost, source->remote_video_rtcp_port);
			janus_streaming_rtsp_latch(source->video_rtcp_fd, source->rtsp_vhost,
				source->remote_video_rtcp_port, (struct sockaddr *)&source->video_rtcp_addr);
		}
	}
	/* Send an RTSP PLAY */
	janus_mutex_lock(&source->rtsp_mutex);
	g_free(source->curldata->buffer);
	source->curldata->buffer = g_malloc0(1);
	source->curldata->size = 0;
	JANUS_LOG(LOG_VERB, "Sending PLAY request...\n");
	curl_easy_setopt(source->curl, CURLOPT_RTSP_STREAM_URI, source->rtsp_url);
	curl_easy_setopt(source->curl, CURLOPT_RANGE, "npt=0.000-");
	curl_easy_setopt(source->curl, CURLOPT_RTSP_REQUEST, (long)CURL_RTSPREQ_PLAY);
	int res = curl_easy_perform(source->curl);
	if(res != CURLE_OK) {
		JANUS_LOG(LOG_ERR, "Couldn't send PLAY request: %s\n", curl_easy_strerror(res));
		janus_mutex_unlock(&source->rtsp_mutex);
		return -1;
	}
	JANUS_LOG(LOG_VERB, "PLAY answer:%s\n", source->curldata->buffer);
	janus_mutex_unlock(&source->rtsp_mutex);
	return 0;
}

/* Helper to create an RTSP source */
janus_streaming_mountpoint *janus_streaming_create_rtsp_source(
		uint64_t id, char *id_str, char *name, char *desc, char *metadata,
		char *url, char *username, char *password,
		gboolean doaudio, int acodec, char *artpmap, char *afmtp,
		gboolean dovideo, int vcodec, char *vrtpmap, char *vfmtp, gboolean bufferkf,
		const janus_network_address *iface,
		gboolean error_on_failure) {
	char id_num[30];
	if(!string_ids) {
		g_snprintf(id_num, sizeof(id_num), "%"SCNu64, id);
		id_str = id_num;
	}
	if(url == NULL) {
		JANUS_LOG(LOG_ERR, "Can't add 'rtsp' stream, missing url...\n");
		return NULL;
	}
	JANUS_LOG(LOG_VERB, "Audio %s, Video %s\n", doaudio ? "enabled" : "NOT enabled", dovideo ? "enabled" : "NOT enabled");

	/* Create an RTP source for the media we'll get */
	char tempname[255];
	if(name == NULL) {
		JANUS_LOG(LOG_VERB, "Missing name, will generate a random one...\n");
		memset(tempname, 0, 255);
		g_snprintf(tempname, 255, "%s", id_str);
	} else if(atoi(name) != 0) {
		JANUS_LOG(LOG_VERB, "Names can't start with a number, prefixing it...\n");
		memset(tempname, 0, 255);
		g_snprintf(tempname, 255, "mp-%s", name);
		name = NULL;
	}
	char *sourcename =  g_strdup(name ? name : tempname);
	char *description = NULL;
	if(desc != NULL) {
		description = g_strdup(desc);
	} else {
		description = g_strdup(name ? name : tempname);
	}

	janus_network_address nil;
	janus_network_address_nullify(&nil);

	/* Create the mountpoint and prepare the source */
	janus_streaming_mountpoint *live_rtsp = g_malloc0(sizeof(janus_streaming_mountpoint));
	live_rtsp->id = id;
	live_rtsp->id_str = g_strdup(id_str);
	live_rtsp->name = sourcename;
	live_rtsp->description = description;
	live_rtsp->metadata = (metadata ? g_strdup(metadata) : NULL);
	live_rtsp->enabled = TRUE;
	live_rtsp->active = FALSE;
	live_rtsp->audio = doaudio;
	live_rtsp->video = dovideo;
	live_rtsp->data = FALSE;
	live_rtsp->streaming_type = janus_streaming_type_live;
	live_rtsp->streaming_source = janus_streaming_source_rtp;
	janus_streaming_rtp_source *live_rtsp_source = g_malloc0(sizeof(janus_streaming_rtp_source));
	live_rtsp_source->rtsp = TRUE;
	live_rtsp_source->rtsp_url = g_strdup(url);
	live_rtsp_source->rtsp_username = username ? g_strdup(username) : NULL;
	live_rtsp_source->rtsp_password = password ? g_strdup(password) : NULL;
	live_rtsp_source->arc = NULL;
	live_rtsp_source->vrc = NULL;
	live_rtsp_source->drc = NULL;
	live_rtsp_source->audio_fd = -1;
	live_rtsp_source->audio_rtcp_fd = -1;
	live_rtsp_source->audio_iface = iface ? *iface : nil;
	live_rtsp_source->video_fd[0] = -1;
	live_rtsp_source->video_fd[1] = -1;
	live_rtsp_source->video_fd[2] = -1;
	live_rtsp_source->video_rtcp_fd = -1;
	live_rtsp_source->video_iface = iface ? *iface : nil;
	live_rtsp_source->data_fd = -1;
	live_rtsp_source->pipefd[0] = -1;
	live_rtsp_source->pipefd[1] = -1;
	pipe(live_rtsp_source->pipefd);
	live_rtsp_source->data_iface = nil;
	live_rtsp_source->keyframe.enabled = bufferkf;
	live_rtsp_source->keyframe.latest_keyframe = NULL;
	live_rtsp_source->keyframe.temp_keyframe = NULL;
	live_rtsp_source->keyframe.temp_ts = 0;
	janus_mutex_init(&live_rtsp_source->keyframe.mutex);
	live_rtsp_source->reconnect_timer = 0;
	janus_mutex_init(&live_rtsp_source->rtsp_mutex);
	live_rtsp->source = live_rtsp_source;
	live_rtsp->source_destroy = (GDestroyNotify) janus_streaming_rtp_source_free;
	live_rtsp->viewers = NULL;
	g_atomic_int_set(&live_rtsp->destroyed, 0);
	janus_refcount_init(&live_rtsp->ref, janus_streaming_mountpoint_free);
	janus_mutex_init(&live_rtsp->mutex);
	/* We may have to override the payload type and/or rtpmap and/or fmtp for audio and/or video */
	live_rtsp->codecs.audio_pt = doaudio ? acodec : -1;
	live_rtsp->codecs.audio_rtpmap = doaudio ? (artpmap ? g_strdup(artpmap) : NULL) : NULL;
	live_rtsp->codecs.audio_fmtp = doaudio ? (afmtp ? g_strdup(afmtp) : NULL) : NULL;
	live_rtsp->codecs.video_pt = dovideo ? vcodec : -1;
	live_rtsp->codecs.video_rtpmap = dovideo ? (vrtpmap ? g_strdup(vrtpmap) : NULL) : NULL;
	live_rtsp->codecs.video_fmtp = dovideo ? (vfmtp ? g_strdup(vfmtp) : NULL) : NULL;
	/* If we need to return an error on failure, try connecting right now */
	if(error_on_failure) {
		/* Now connect to the RTSP server */
		if(janus_streaming_rtsp_connect_to_server(live_rtsp) < 0) {
			/* Error connecting, get rid of the mountpoint */
			janus_mutex_lock(&mountpoints_mutex);
			g_hash_table_remove(mountpoints_temp, &id);
			janus_mutex_unlock(&mountpoints_mutex);
			janus_refcount_decrease(&live_rtsp->ref);
			return NULL;
		}
		/* Send an RTSP PLAY, now */
		if(janus_streaming_rtsp_play(live_rtsp_source) < 0) {
			/* Error trying to play, get rid of the mountpoint */
			janus_mutex_lock(&mountpoints_mutex);
			g_hash_table_remove(mountpoints_temp, &id);
			janus_mutex_unlock(&mountpoints_mutex);
			janus_refcount_decrease(&live_rtsp->ref);
			return NULL;
		}
	}
	/* Start the thread that will receive the media packets */
	GError *error = NULL;
	char tname[16];
	g_snprintf(tname, sizeof(tname), "mp %s", live_rtsp->id_str);
	janus_refcount_increase(&live_rtsp->ref);
	live_rtsp->thread = g_thread_try_new(tname, &janus_streaming_relay_thread, live_rtsp, &error);
	if(error != NULL) {
		JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the RTSP thread...\n",
			error->code, error->message ? error->message : "??");
		g_error_free(error);
		janus_mutex_lock(&mountpoints_mutex);
		g_hash_table_remove(mountpoints_temp, &id);
		janus_mutex_unlock(&mountpoints_mutex);
		janus_refcount_decrease(&live_rtsp->ref);	/* This is for the failed thread */
		janus_refcount_decrease(&live_rtsp->ref);
		return NULL;
	}
	janus_mutex_lock(&mountpoints_mutex);
	g_hash_table_insert(mountpoints,
		string_ids ? (gpointer)g_strdup(live_rtsp->id_str) : (gpointer)janus_uint64_dup(live_rtsp->id),
		live_rtsp);
	g_hash_table_remove(mountpoints_temp, string_ids ? (gpointer)live_rtsp->id_str : (gpointer)&live_rtsp->id);
	janus_mutex_unlock(&mountpoints_mutex);
	return live_rtsp;
}
#else
/* Helper to create an RTSP source */
janus_streaming_mountpoint *janus_streaming_create_rtsp_source(
		uint64_t id, char *id_str, char *name, char *desc, char *metadata,
		char *url, char *username, char *password,
		gboolean doaudio, int acodec, char *audiortpmap, char *audiofmtp,
		gboolean dovideo, int vcodec, char *videortpmap, char *videofmtp, gboolean bufferkf,
		const janus_network_address *iface,
		gboolean error_on_failure) {
	JANUS_LOG(LOG_ERR, "RTSP need libcurl\n");
	return NULL;
}
#endif

/* Thread to send RTP packets from a file (on demand) */
static void *janus_streaming_ondemand_thread(void *data) {
	JANUS_LOG(LOG_VERB, "Filesource (on demand) RTP thread starting...\n");
	janus_streaming_session *session = (janus_streaming_session *)data;
	if(!session) {
		JANUS_LOG(LOG_ERR, "Invalid session!\n");
		g_thread_unref(g_thread_self());
		return NULL;
	}
	janus_streaming_mountpoint *mountpoint = session->mountpoint;
	if(!mountpoint) {
		JANUS_LOG(LOG_ERR, "Invalid mountpoint!\n");
		janus_refcount_decrease(&session->ref);
		g_thread_unref(g_thread_self());
		return NULL;
	}
	if(mountpoint->streaming_source != janus_streaming_source_file) {
		JANUS_LOG(LOG_ERR, "[%s] Not an file source mountpoint!\n", mountpoint->name);
		janus_refcount_decrease(&session->ref);
		janus_refcount_decrease(&mountpoint->ref);
		g_thread_unref(g_thread_self());
		return NULL;
	}
	if(mountpoint->streaming_type != janus_streaming_type_on_demand) {
		JANUS_LOG(LOG_ERR, "[%s] Not an on-demand file source mountpoint!\n", mountpoint->name);
		janus_refcount_decrease(&session->ref);
		janus_refcount_decrease(&mountpoint->ref);
		g_thread_unref(g_thread_self());
		return NULL;
	}
	janus_streaming_file_source *source = mountpoint->source;
	if(source == NULL || source->filename == NULL) {
		JANUS_LOG(LOG_ERR, "[%s] Invalid file source mountpoint!\n", mountpoint->name);
		janus_refcount_decrease(&session->ref);
		janus_refcount_decrease(&mountpoint->ref);
		g_thread_unref(g_thread_self());
		return NULL;
	}
	JANUS_LOG(LOG_VERB, "[%s] Opening file source %s...\n", mountpoint->name, source->filename);
	FILE *audio = fopen(source->filename, "rb");
	if(!audio) {
		JANUS_LOG(LOG_ERR, "[%s] Ooops, audio file missing!\n", mountpoint->name);
		janus_refcount_decrease(&session->ref);
		janus_refcount_decrease(&mountpoint->ref);
		g_thread_unref(g_thread_self());
		return NULL;
	}
	char *name = g_strdup(mountpoint->name ? mountpoint->name : "??");
	JANUS_LOG(LOG_VERB, "[%s] Streaming audio file: %s\n", name, source->filename);

#ifdef HAVE_LIBOGG
	/* Make sure that, if this is an .opus file, we can open it */
	janus_streaming_opus_context opusctx = { 0 };
	if(source->opus) {
		opusctx.name = name;
		opusctx.filename = source->filename;
		opusctx.file = audio;
		if(janus_streaming_opus_context_init(&opusctx) < 0) {
			g_free(name);
			fclose(audio);
			janus_refcount_decrease(&session->ref);
			janus_refcount_decrease(&mountpoint->ref);
			g_thread_unref(g_thread_self());
			return NULL;
		}
	}
#endif

	/* Buffer */
	char buf[1500];
	memset(buf, 0, sizeof(buf));
	/* Set up RTP */
	gint16 seq = 1;
	gint32 ts = 0;
	janus_rtp_header *header = (janus_rtp_header *)buf;
	header->version = 2;
	header->markerbit = 1;
	header->type = mountpoint->codecs.audio_pt;
	header->seq_number = htons(seq);
	header->timestamp = htonl(ts);
	header->ssrc = htonl(1);	/* The gateway will fix this anyway */
	/* Timer */
	struct timeval now, before;
	gettimeofday(&before, NULL);
	now.tv_sec = before.tv_sec;
	now.tv_usec = before.tv_usec;
	time_t passed, d_s, d_us;
	/* Loop */
	gint read = 0, plen = (sizeof(buf)-RTP_HEADER_SIZE);
	janus_streaming_rtp_relay_packet packet;
	while(!g_atomic_int_get(&stopping) && !g_atomic_int_get(&mountpoint->destroyed) &&
			!g_atomic_int_get(&session->stopping) && !g_atomic_int_get(&session->destroyed)) {
		/* See if it's time to prepare a frame */
		gettimeofday(&now, NULL);
		d_s = now.tv_sec - before.tv_sec;
		d_us = now.tv_usec - before.tv_usec;
		if(d_us < 0) {
			d_us += 1000000;
			--d_s;
		}
		passed = d_s*1000000 + d_us;
		if(passed < 18000) {	/* Let's wait about 18ms */
			g_usleep(5000);
			continue;
		}
		/* Update the reference time */
		before.tv_usec += 20000;
		if(before.tv_usec > 1000000) {
			before.tv_sec++;
			before.tv_usec -= 1000000;
		}
		/* If not started or paused, wait some more */
		if(!g_atomic_int_get(&session->started) || g_atomic_int_get(&session->paused) || !mountpoint->enabled)
			continue;
		if(source->opus) {
#ifdef HAVE_LIBOGG
			/* Get the next frame from the Opus file */
			read = janus_streaming_opus_context_read(&opusctx, buf + RTP_HEADER_SIZE, plen);
#endif
		} else {
			/* Read frame from file... */
			read = fread(buf + RTP_HEADER_SIZE, sizeof(char), 160, audio);
			if(feof(audio)) {
				/* FIXME We're doing this forever... should this be configurable? */
				JANUS_LOG(LOG_VERB, "[%s] Rewind! (%s)\n", name, source->filename);
				fseek(audio, 0, SEEK_SET);
				continue;
			}
		}
		if(read < 0)
			break;
		if(mountpoint->active == FALSE)
			mountpoint->active = TRUE;
		/* Relay to the listener */
		packet.data = header;
		packet.length = RTP_HEADER_SIZE + read;
		packet.is_rtp = TRUE;
		packet.is_video = FALSE;
		packet.is_keyframe = FALSE;
		/* Backup the actual timestamp and sequence number */
		packet.timestamp = ntohl(packet.data->timestamp);
		packet.seq_number = ntohs(packet.data->seq_number);
		/* Go! */
		janus_streaming_relay_rtp_packet(session, &packet);
		/* Update header */
		seq++;
		header->seq_number = htons(seq);
		ts += (source->opus ? 960 : 160);
		header->timestamp = htonl(ts);
		header->markerbit = 0;
	}
	JANUS_LOG(LOG_VERB, "[%s] Leaving filesource (ondemand) thread\n", name);
#ifdef HAVE_LIBOGG
	if(source->opus)
		janus_streaming_opus_context_cleanup(&opusctx);
#endif
	g_free(name);
	fclose(audio);
	janus_refcount_decrease(&session->ref);
	janus_refcount_decrease(&mountpoint->ref);
	g_thread_unref(g_thread_self());
	return NULL;
}

/* Thread to send RTP packets from a file (live) */
static void *janus_streaming_filesource_thread(void *data) {
	JANUS_LOG(LOG_VERB, "Filesource (live) thread starting...\n");
	janus_streaming_mountpoint *mountpoint = (janus_streaming_mountpoint *)data;
	if(!mountpoint) {
		JANUS_LOG(LOG_ERR, "Invalid mountpoint!\n");
		return NULL;
	}
	if(mountpoint->streaming_source != janus_streaming_source_file) {
		JANUS_LOG(LOG_ERR, "[%s] Not an file source mountpoint!\n", mountpoint->name);
		janus_refcount_decrease(&mountpoint->ref);
		return NULL;
	}
	if(mountpoint->streaming_type != janus_streaming_type_live) {
		JANUS_LOG(LOG_ERR, "[%s] Not a live file source mountpoint!\n", mountpoint->name);
		janus_refcount_decrease(&mountpoint->ref);
		return NULL;
	}
	janus_streaming_file_source *source = mountpoint->source;
	if(source == NULL || source->filename == NULL) {
		JANUS_LOG(LOG_ERR, "[%s] Invalid file source mountpoint!\n", mountpoint->name);
		janus_refcount_decrease(&mountpoint->ref);
		return NULL;
	}
	JANUS_LOG(LOG_VERB, "[%s] Opening file source %s...\n", mountpoint->name, source->filename);
	FILE *audio = fopen(source->filename, "rb");
	if(!audio) {
		JANUS_LOG(LOG_ERR, "[%s] Ooops, audio file missing!\n", mountpoint->name);
		janus_refcount_decrease(&mountpoint->ref);
		return NULL;
	}
	char *name = g_strdup(mountpoint->name ? mountpoint->name : "??");
	JANUS_LOG(LOG_VERB, "[%s] Streaming audio file: %s\n", mountpoint->name, source->filename);

#ifdef HAVE_LIBOGG
	/* Make sure that, if this is an .opus file, we can open it */
	janus_streaming_opus_context opusctx = { 0 };
	if(source->opus) {
		opusctx.name = name;
		opusctx.filename = source->filename;
		opusctx.file = audio;
		if(janus_streaming_opus_context_init(&opusctx) < 0) {
			g_free(name);
			fclose(audio);
			janus_refcount_decrease(&mountpoint->ref);
			g_thread_unref(g_thread_self());
			return NULL;
		}
	}
#endif

	/* Buffer */
	char buf[1500];
	memset(buf, 0, sizeof(buf));
	/* Set up RTP */
	gint16 seq = 1;
	gint32 ts = 0;
	janus_rtp_header *header = (janus_rtp_header *)buf;
	header->version = 2;
	header->markerbit = 1;
	header->type = mountpoint->codecs.audio_pt;
	header->seq_number = htons(seq);
	header->timestamp = htonl(ts);
	header->ssrc = htonl(1);	/* The Janus core will fix this anyway */
	/* Timer */
	struct timeval now, before;
	gettimeofday(&before, NULL);
	now.tv_sec = before.tv_sec;
	now.tv_usec = before.tv_usec;
	time_t passed, d_s, d_us;
	/* Loop */
	gint read = 0, plen = (sizeof(buf)-RTP_HEADER_SIZE);
	janus_streaming_rtp_relay_packet packet;
	while(!g_atomic_int_get(&stopping) && !g_atomic_int_get(&mountpoint->destroyed)) {
		/* See if it's time to prepare a frame */
		gettimeofday(&now, NULL);
		d_s = now.tv_sec - before.tv_sec;
		d_us = now.tv_usec - before.tv_usec;
		if(d_us < 0) {
			d_us += 1000000;
			--d_s;
		}
		passed = d_s*1000000 + d_us;
		if(passed < 18000) {	/* Let's wait about 18ms */
			g_usleep(5000);
			continue;
		}
		/* Update the reference time */
		before.tv_usec += 20000;
		if(before.tv_usec > 1000000) {
			before.tv_sec++;
			before.tv_usec -= 1000000;
		}
		/* If paused, wait some more */
		if(!mountpoint->enabled)
			continue;
		if(source->opus) {
#ifdef HAVE_LIBOGG
			/* Get the next frame from the Opus file */
			read = janus_streaming_opus_context_read(&opusctx, buf + RTP_HEADER_SIZE, plen);
#endif
		} else {
			/* Read frame from file... */
			read = fread(buf + RTP_HEADER_SIZE, sizeof(char), 160, audio);
			if(feof(audio)) {
				/* FIXME We're doing this forever... should this be configurable? */
				JANUS_LOG(LOG_VERB, "[%s] Rewind! (%s)\n", name, source->filename);
				fseek(audio, 0, SEEK_SET);
				continue;
			}
		}
		if(read < 0)
			break;
		if(mountpoint->active == FALSE)
			mountpoint->active = TRUE;
		/* Relay on all sessions */
		packet.data = header;
		packet.length = RTP_HEADER_SIZE + read;
		packet.is_rtp = TRUE;
		packet.is_video = FALSE;
		packet.is_keyframe = FALSE;
		/* Backup the actual timestamp and sequence number */
		packet.timestamp = ntohl(packet.data->timestamp);
		packet.seq_number = ntohs(packet.data->seq_number);
		/* Go! */
		janus_mutex_lock_nodebug(&mountpoint->mutex);
		g_list_foreach(mountpoint->viewers, janus_streaming_relay_rtp_packet, &packet);
		janus_mutex_unlock_nodebug(&mountpoint->mutex);
		/* Update header */
		seq++;
		header->seq_number = htons(seq);
		ts += (source->opus ? 960 : 160);
		header->timestamp = htonl(ts);
		header->markerbit = 0;
	}
	JANUS_LOG(LOG_VERB, "[%s] Leaving filesource (live) thread\n", name);
#ifdef HAVE_LIBOGG
	if(source->opus)
		janus_streaming_opus_context_cleanup(&opusctx);
#endif
	g_free(name);
	fclose(audio);
	janus_refcount_decrease(&mountpoint->ref);
	return NULL;
}

/* Thread to relay RTP frames coming from gstreamer/ffmpeg/others */
static void *janus_streaming_relay_thread(void *data) {
	JANUS_LOG(LOG_VERB, "Starting streaming relay thread\n");
	janus_streaming_mountpoint *mountpoint = (janus_streaming_mountpoint *)data;
	if(!mountpoint) {
		JANUS_LOG(LOG_ERR, "Invalid mountpoint!\n");
		return NULL;
	}
	if(mountpoint->streaming_source != janus_streaming_source_rtp) {
		janus_refcount_decrease(&mountpoint->ref);
		JANUS_LOG(LOG_ERR, "[%s] Not an RTP source mountpoint!\n", mountpoint->name);
		return NULL;
	}
	janus_streaming_rtp_source *source = mountpoint->source;
	if(source == NULL) {
		JANUS_LOG(LOG_ERR, "[%s] Invalid RTP source mountpoint!\n", mountpoint->name);
		janus_refcount_decrease(&mountpoint->ref);
		return NULL;
	}

	/* Add a reference to the helper threads, if needed */
	if(mountpoint->helper_threads > 0) {
		GList *l = mountpoint->threads;
		while(l) {
			janus_streaming_helper *ht = (janus_streaming_helper *)l->data;
			janus_refcount_increase(&ht->ref);
			l = l->next;
		}
	}

	int audio_fd = source->audio_fd;
	int video_fd[3] = {source->video_fd[0], source->video_fd[1], source->video_fd[2]};
	int data_fd = source->data_fd;
	int pipe_fd = source->pipefd[0];
	int audio_rtcp_fd = source->audio_rtcp_fd;
	int video_rtcp_fd = source->video_rtcp_fd;
	char *name = g_strdup(mountpoint->name ? mountpoint->name : "??");
	/* Needed to fix seq and ts */
	uint32_t ssrc = 0, a_last_ssrc = 0, v_last_ssrc[3] = {0, 0, 0};
	/* File descriptors */
	socklen_t addrlen;
	struct sockaddr_storage remote;
	int resfd = 0, bytes = 0;
	struct pollfd fds[8];
	char buffer[1500];
	memset(buffer, 0, 1500);
#ifdef HAVE_LIBCURL
	/* In case this is an RTSP restreamer, we may have to send keep-alives from time to time */
	gint64 now = janus_get_monotonic_time(), before = now, ka_timeout = 0;
	if(source->rtsp) {
		source->reconnect_timer = now;
		ka_timeout = ((gint64)source->ka_timeout*G_USEC_PER_SEC)/2;
	}
#endif
	/* Loop */
	int num = 0;
	janus_streaming_rtp_relay_packet packet;
	while(!g_atomic_int_get(&stopping) && !g_atomic_int_get(&mountpoint->destroyed)) {
#ifdef HAVE_LIBCURL
		/* Let's check regularly if the RTSP server seems to be gone */
		if(source->rtsp) {
			if(source->reconnecting) {
				/* We're still reconnecting, wait some more */
				g_usleep(250000);
				continue;
			}
			now = janus_get_monotonic_time();
			if(!source->reconnecting && (now - source->reconnect_timer > 5*G_USEC_PER_SEC)) {
				/* 5 seconds passed and no media? Assume the RTSP server has gone and schedule a reconnect */
				JANUS_LOG(LOG_WARN, "[%s] %"SCNi64"s passed with no media, trying to reconnect the RTSP stream\n",
					name, (now - source->reconnect_timer)/G_USEC_PER_SEC);
				audio_fd = -1;
				video_fd[0] = -1;
				video_fd[1] = -1;
				video_fd[2] = -1;
				data_fd = -1;
				source->reconnect_timer = now;
				source->reconnecting = TRUE;
				/* Let's clean up the source first */
				curl_easy_cleanup(source->curl);
				source->curl = NULL;
				if(source->curldata)
					g_free(source->curldata->buffer);
				g_free(source->curldata);
				source->curldata = NULL;
				if(source->audio_fd > -1) {
					close(source->audio_fd);
				}
				source->audio_fd = -1;
				if(source->video_fd[0] > -1) {
					close(source->video_fd[0]);
				}
				source->video_fd[0] = -1;
				if(source->video_fd[1] > -1) {
					close(source->video_fd[1]);
				}
				source->video_fd[1] = -1;
				if(source->video_fd[2] > -1) {
					close(source->video_fd[2]);
				}
				source->video_fd[2] = -1;
				if(source->data_fd > -1) {
					close(source->data_fd);
				}
				source->data_fd = -1;
				if(source->audio_rtcp_fd > -1) {
					close(source->audio_rtcp_fd);
				}
				source->audio_rtcp_fd = -1;
				if(source->video_rtcp_fd > -1) {
					close(source->video_rtcp_fd);
				}
				source->video_rtcp_fd = -1;
				/* Now let's try to reconnect */
				if(janus_streaming_rtsp_connect_to_server(mountpoint) < 0) {
					/* Reconnection failed? Let's try again later */
					JANUS_LOG(LOG_WARN, "[%s] Reconnection of the RTSP stream failed, trying again in a few seconds...\n", name);
				} else {
					/* We're connected, let's send a PLAY */
					if(janus_streaming_rtsp_play(source) < 0) {
						/* Error trying to play? Let's try again later */
						JANUS_LOG(LOG_WARN, "[%s] RTSP PLAY failed, trying again in a few seconds...\n", name);
					} else {
						/* Everything should be back to normal, let's update the file descriptors */
						JANUS_LOG(LOG_INFO, "[%s] Reconnected to the RTSP server, streaming again\n", name);
						audio_fd = source->audio_fd;
						video_fd[0] = source->video_fd[0];
						data_fd = source->data_fd;
						audio_rtcp_fd = source->audio_rtcp_fd;
						video_rtcp_fd = source->video_rtcp_fd;
						ka_timeout = ((gint64)source->ka_timeout*G_USEC_PER_SEC)/2;
					}
				}
				source->reconnect_timer = janus_get_monotonic_time();
				source->reconnecting = FALSE;
				continue;
			}
		}
		if(audio_fd < 0 && video_fd[0] < 0 && video_fd[1] < 0 && video_fd[2] < 0 && data_fd < 0) {
			/* No socket, we may be in the process of reconnecting, or waiting to reconnect */
			g_usleep(5000000);
			continue;
		}
		/* We may also need to occasionally send a OPTIONS request as a keep-alive */
		if(ka_timeout > 0) {
			/* Let's be conservative and send a OPTIONS when half of the timeout has passed */
			now = janus_get_monotonic_time();
			if(now-before > ka_timeout && source->curldata) {
				JANUS_LOG(LOG_VERB, "[%s] %"SCNi64"s passed, sending OPTIONS\n", name, (now-before)/G_USEC_PER_SEC);
				before = now;
				/* Send an RTSP OPTIONS */
				janus_mutex_lock(&source->rtsp_mutex);
				g_free(source->curldata->buffer);
				source->curldata->buffer = g_malloc0(1);
				source->curldata->size = 0;
				curl_easy_setopt(source->curl, CURLOPT_RTSP_STREAM_URI, source->rtsp_url);
				curl_easy_setopt(source->curl, CURLOPT_RTSP_REQUEST, (long)CURL_RTSPREQ_OPTIONS);
				resfd = curl_easy_perform(source->curl);
				if(resfd != CURLE_OK) {
					JANUS_LOG(LOG_ERR, "[%s] Couldn't send OPTIONS request: %s\n", name, curl_easy_strerror(resfd));
				}
				janus_mutex_unlock(&source->rtsp_mutex);
			}
		}
#endif
		/* Any PLI and/or REMB we should send back to the source? */
		if(g_atomic_int_get(&source->need_pli))
			janus_streaming_rtcp_pli_send(source);
		if(source->video_rtcp_fd > -1 && source->lowest_bitrate > 0) {
			gint64 now = janus_get_monotonic_time();
			if(source->remb_latest == 0)
				source->remb_latest = now;
			else if(now - source->remb_latest >= G_USEC_PER_SEC)
				janus_streaming_rtcp_remb_send(source);
		}
		/* Prepare poll */
		num = 0;
		if(audio_fd != -1) {
			fds[num].fd = audio_fd;
			fds[num].events = POLLIN;
			fds[num].revents = 0;
			num++;
		}
		if(video_fd[0] != -1) {
			fds[num].fd = video_fd[0];
			fds[num].events = POLLIN;
			fds[num].revents = 0;
			num++;
		}
		if(video_fd[1] != -1) {
			fds[num].fd = video_fd[1];
			fds[num].events = POLLIN;
			fds[num].revents = 0;
			num++;
		}
		if(video_fd[2] != -1) {
			fds[num].fd = video_fd[2];
			fds[num].events = POLLIN;
			fds[num].revents = 0;
			num++;
		}
		if(data_fd != -1) {
			fds[num].fd = data_fd;
			fds[num].events = POLLIN;
			fds[num].revents = 0;
			num++;
		}
		if(pipe_fd != -1) {
			fds[num].fd = pipe_fd;
			fds[num].events = POLLIN;
			fds[num].revents = 0;
			num++;
		}
		if(audio_rtcp_fd != -1) {
			fds[num].fd = audio_rtcp_fd;
			fds[num].events = POLLIN;
			fds[num].revents = 0;
			num++;
		}
		if(video_rtcp_fd != -1) {
			fds[num].fd = video_rtcp_fd;
			fds[num].events = POLLIN;
			fds[num].revents = 0;
			num++;
		}
		/* Wait for some data */
		resfd = poll(fds, num, 1000);
		if(resfd < 0) {
			if(errno == EINTR) {
				JANUS_LOG(LOG_HUGE, "[%s] Got an EINTR (%s), ignoring...\n", name, strerror(errno));
				continue;
			}
			JANUS_LOG(LOG_ERR, "[%s] Error polling... %d (%s)\n", name, errno, strerror(errno));
			mountpoint->enabled = FALSE;
			janus_mutex_lock(&source->rec_mutex);
			if(source->arc) {
				janus_recorder_close(source->arc);
				JANUS_LOG(LOG_INFO, "[%s] Closed audio recording %s\n", mountpoint->name, source->arc->filename ? source->arc->filename : "??");
				janus_recorder *tmp = source->arc;
				source->arc = NULL;
				janus_recorder_destroy(tmp);
			}
			if(source->vrc) {
				janus_recorder_close(source->vrc);
				JANUS_LOG(LOG_INFO, "[%s] Closed video recording %s\n", mountpoint->name, source->vrc->filename ? source->vrc->filename : "??");
				janus_recorder *tmp = source->vrc;
				source->vrc = NULL;
				janus_recorder_destroy(tmp);
			}
			if(source->drc) {
				janus_recorder_close(source->drc);
				JANUS_LOG(LOG_INFO, "[%s] Closed data recording %s\n", mountpoint->name, source->drc->filename ? source->drc->filename : "??");
				janus_recorder *tmp = source->drc;
				source->drc = NULL;
				janus_recorder_destroy(tmp);
			}
			janus_mutex_unlock(&source->rec_mutex);
			break;
		} else if(resfd == 0) {
			/* No data, keep going */
			continue;
		}
		int i = 0;
		for(i=0; i<num; i++) {
			if(fds[i].revents & (POLLERR | POLLHUP)) {
				/* Socket error? */
				JANUS_LOG(LOG_ERR, "[%s] Error polling: %s... %d (%s)\n", name,
					fds[i].revents & POLLERR ? "POLLERR" : "POLLHUP", errno, strerror(errno));
				mountpoint->enabled = FALSE;
				janus_mutex_lock(&source->rec_mutex);
				if(source->arc) {
					janus_recorder_close(source->arc);
					JANUS_LOG(LOG_INFO, "[%s] Closed audio recording %s\n", mountpoint->name, source->arc->filename ? source->arc->filename : "??");
					janus_recorder *tmp = source->arc;
					source->arc = NULL;
					janus_recorder_destroy(tmp);
				}
				if(source->vrc) {
					janus_recorder_close(source->vrc);
					JANUS_LOG(LOG_INFO, "[%s] Closed video recording %s\n", mountpoint->name, source->vrc->filename ? source->vrc->filename : "??");
					janus_recorder *tmp = source->vrc;
					source->vrc = NULL;
					janus_recorder_destroy(tmp);
				}
				if(source->drc) {
					janus_recorder_close(source->drc);
					JANUS_LOG(LOG_INFO, "[%s] Closed data recording %s\n", mountpoint->name, source->drc->filename ? source->drc->filename : "??");
					janus_recorder *tmp = source->drc;
					source->drc = NULL;
					janus_recorder_destroy(tmp);
				}
				janus_mutex_unlock(&source->rec_mutex);
				break;
			} else if(fds[i].revents & POLLIN) {
				/* Got an RTP or data packet */
				if(pipe_fd != -1 && fds[i].fd == pipe_fd) {
					/* We're done here */
					int code = 0;
					bytes = read(pipe_fd, &code, sizeof(int));
					JANUS_LOG(LOG_VERB, "[%s] Interrupting mountpoint\n", mountpoint->name);
					break;
				} else if(audio_fd != -1 && fds[i].fd == audio_fd) {
					/* Got something audio (RTP) */
					if(mountpoint->active == FALSE)
						mountpoint->active = TRUE;
					gint64 now = janus_get_monotonic_time();
#ifdef HAVE_LIBCURL
					source->reconnect_timer = now;
#endif
					addrlen = sizeof(remote);
					bytes = recvfrom(audio_fd, buffer, 1500, 0, (struct sockaddr *)&remote, &addrlen);
					if(bytes < 0 || !janus_is_rtp(buffer, bytes)) {
						/* Failed to read or not an RTP packet? */
						continue;
					}
					janus_rtp_header *rtp = (janus_rtp_header *)buffer;
					ssrc = ntohl(rtp->ssrc);
					if(source->rtp_collision > 0 && a_last_ssrc && ssrc != a_last_ssrc &&
							(now-source->last_received_audio) < (gint64)1000*source->rtp_collision) {
						JANUS_LOG(LOG_WARN, "[%s] RTP collision on audio mountpoint, dropping packet (ssrc=%"SCNu32")\n", name, ssrc);
						continue;
					}
					source->last_received_audio = now;
					//~ JANUS_LOG(LOG_VERB, "************************\nGot %d bytes on the audio channel...\n", bytes);
					/* Do we have a new stream? */
					if(ssrc != a_last_ssrc) {
						source->audio_ssrc = a_last_ssrc = ssrc;
						JANUS_LOG(LOG_INFO, "[%s] New audio stream! (ssrc=%"SCNu32")\n", name, a_last_ssrc);
					}
					/* If paused, ignore this packet */
					if(!mountpoint->enabled && !source->arc)
						continue;
					/* Is this SRTP? */
					if(source->is_srtp) {
						int buflen = bytes;
						srtp_err_status_t res = srtp_unprotect(source->srtp_ctx, buffer, &buflen);
						//~ if(res != srtp_err_status_ok && res != srtp_err_status_replay_fail && res != srtp_err_status_replay_old) {
						if(res != srtp_err_status_ok) {
							guint32 timestamp = ntohl(rtp->timestamp);
							guint16 seq = ntohs(rtp->seq_number);
							JANUS_LOG(LOG_ERR, "[%s] Audio SRTP unprotect error: %s (len=%d-->%d, ts=%"SCNu32", seq=%"SCNu16")\n",
								name, janus_srtp_error_str(res), bytes, buflen, timestamp, seq);
							continue;
						}
						bytes = buflen;
					}
					//~ JANUS_LOG(LOG_VERB, " ... parsed RTP packet (ssrc=%u, pt=%u, seq=%u, ts=%u)...\n",
						//~ ntohl(rtp->ssrc), rtp->type, ntohs(rtp->seq_number), ntohl(rtp->timestamp));
					/* Relay on all sessions */
					packet.data = rtp;
					packet.length = bytes;
					packet.is_rtp = TRUE;
					packet.is_video = FALSE;
					packet.is_keyframe = FALSE;
					packet.data->type = mountpoint->codecs.audio_pt;
					/* Is there a recorder? */
					janus_rtp_header_update(packet.data, &source->context[0], FALSE, 0);
					if(source->askew) {
						int ret = janus_rtp_skew_compensate_audio(packet.data, &source->context[0], now);
						if(ret < 0) {
							JANUS_LOG(LOG_WARN, "[%s] Dropping %d packets, audio source clock is too fast (ssrc=%"SCNu32")\n",
								name, -ret, a_last_ssrc);
							continue;
						} else if(ret > 0) {
							JANUS_LOG(LOG_WARN, "[%s] Jumping %d RTP sequence numbers, audio source clock is too slow (ssrc=%"SCNu32")\n",
								name, ret, a_last_ssrc);
						}
					}
					if(source->arc) {
						packet.data->ssrc = htonl((uint32_t)mountpoint->id);
						janus_recorder_save_frame(source->arc, buffer, bytes);
					}
					if(mountpoint->enabled) {
						packet.data->ssrc = htonl(ssrc);
						/* Backup the actual timestamp and sequence number set by the restreamer, in case switching is involved */
						packet.timestamp = ntohl(packet.data->timestamp);
						packet.seq_number = ntohs(packet.data->seq_number);
						/* Go! */

						janus_mutex_lock(&mountpoint->mutex);
						g_list_foreach(mountpoint->helper_threads == 0 ? mountpoint->viewers : mountpoint->threads,
							mountpoint->helper_threads == 0 ? janus_streaming_relay_rtp_packet : janus_streaming_helper_rtprtcp_packet,
							&packet);
						janus_mutex_unlock(&mountpoint->mutex);
					}
					continue;
				} else if((video_fd[0] != -1 && fds[i].fd == video_fd[0]) ||
						(video_fd[1] != -1 && fds[i].fd == video_fd[1]) ||
						(video_fd[2] != -1 && fds[i].fd == video_fd[2])) {
					/* Got something video (RTP) */
					int index = -1;
					if(fds[i].fd == video_fd[0])
						index = 0;
					else if(fds[i].fd == video_fd[1])
						index = 1;
					else if(fds[i].fd == video_fd[2])
						index = 2;
					if(mountpoint->active == FALSE)
						mountpoint->active = TRUE;
					gint64 now = janus_get_monotonic_time();
#ifdef HAVE_LIBCURL
					source->reconnect_timer = now;
#endif
					addrlen = sizeof(remote);
					bytes = recvfrom(fds[i].fd, buffer, 1500, 0, (struct sockaddr *)&remote, &addrlen);
					if(bytes < 0 || !janus_is_rtp(buffer, bytes)) {
						/* Failed to read or not an RTP packet? */
						continue;
					}
					janus_rtp_header *rtp = (janus_rtp_header *)buffer;
					ssrc = ntohl(rtp->ssrc);
					if(source->rtp_collision > 0 && v_last_ssrc[index] && ssrc != v_last_ssrc[index] &&
							(now-source->last_received_video) < (gint64)1000*source->rtp_collision) {
						JANUS_LOG(LOG_WARN, "[%s] RTP collision on video mountpoint, dropping packet (ssrc=%"SCNu32")\n",
							name, ssrc);
						continue;
					}
					source->last_received_video = now;
					//~ JANUS_LOG(LOG_VERB, "************************\nGot %d bytes on the video channel...\n", bytes);
					/* Do we have a new stream? */
					if(ssrc != v_last_ssrc[index]) {
						v_last_ssrc[index] = ssrc;
						if(index == 0)
							source->video_ssrc = ssrc;
						JANUS_LOG(LOG_INFO, "[%s] New video stream! (ssrc=%"SCNu32", index %d)\n",
							name, v_last_ssrc[index], index);
					}
					/* Is this SRTP? */
					if(source->is_srtp) {
						int buflen = bytes;
						srtp_err_status_t res = srtp_unprotect(source->srtp_ctx, buffer, &buflen);
						//~ if(res != srtp_err_status_ok && res != srtp_err_status_replay_fail && res != srtp_err_status_replay_old) {
						if(res != srtp_err_status_ok) {
							guint32 timestamp = ntohl(rtp->timestamp);
							guint16 seq = ntohs(rtp->seq_number);
							JANUS_LOG(LOG_ERR, "[%s] Video SRTP unprotect error: %s (len=%d-->%d, ts=%"SCNu32", seq=%"SCNu16")\n",
								name, janus_srtp_error_str(res), bytes, buflen, timestamp, seq);
							continue;
						}
						bytes = buflen;
					}
					/* First of all, let's check if this is (part of) a keyframe that we may need to save it for future reference */
					if(source->keyframe.enabled) {
						if(source->keyframe.temp_ts > 0 && ntohl(rtp->timestamp) != source->keyframe.temp_ts) {
							/* We received the last part of the keyframe, get rid of the old one and use this from now on */
							JANUS_LOG(LOG_HUGE, "[%s] ... ... last part of keyframe received! ts=%"SCNu32", %d packets\n",
								name, source->keyframe.temp_ts, g_list_length(source->keyframe.temp_keyframe));
							source->keyframe.temp_ts = 0;
							janus_mutex_lock(&source->keyframe.mutex);
							if(source->keyframe.latest_keyframe != NULL)
								g_list_free_full(source->keyframe.latest_keyframe, (GDestroyNotify)janus_streaming_rtp_relay_packet_free);
							source->keyframe.latest_keyframe = source->keyframe.temp_keyframe;
							source->keyframe.temp_keyframe = NULL;
							janus_mutex_unlock(&source->keyframe.mutex);
						} else if(ntohl(rtp->timestamp) == source->keyframe.temp_ts) {
							/* Part of the keyframe we're currently saving, store */
							janus_mutex_lock(&source->keyframe.mutex);
							JANUS_LOG(LOG_HUGE, "[%s] ... other part of keyframe received! ts=%"SCNu32"\n", name, source->keyframe.temp_ts);
							janus_streaming_rtp_relay_packet *pkt = g_malloc0(sizeof(janus_streaming_rtp_relay_packet));
							pkt->data = g_malloc(bytes);
							memcpy(pkt->data, buffer, bytes);
							pkt->data->ssrc = htons(1);
							pkt->data->type = mountpoint->codecs.video_pt;
							pkt->is_rtp = TRUE;
							pkt->is_video = TRUE;
							pkt->is_keyframe = TRUE;
							pkt->length = bytes;
							pkt->timestamp = source->keyframe.temp_ts;
							pkt->seq_number = ntohs(rtp->seq_number);
							source->keyframe.temp_keyframe = g_list_append(source->keyframe.temp_keyframe, pkt);
							janus_mutex_unlock(&source->keyframe.mutex);
						} else {
							gboolean kf = FALSE;
							/* Parse RTP header first */
							janus_rtp_header *header = (janus_rtp_header *)buffer;
							guint32 timestamp = ntohl(header->timestamp);
							guint16 seq = ntohs(header->seq_number);
							JANUS_LOG(LOG_HUGE, "Checking if packet (size=%d, seq=%"SCNu16", ts=%"SCNu32") is a key frame...\n",
								bytes, seq, timestamp);
							int plen = 0;
							char *payload = janus_rtp_payload(buffer, bytes, &plen);
							if(payload) {
								switch(mountpoint->codecs.video_codec) {
									case JANUS_VIDEOCODEC_VP8:
										kf = janus_vp8_is_keyframe(payload, plen);
										break;
									case JANUS_VIDEOCODEC_VP9:
										kf = janus_vp9_is_keyframe(payload, plen);
										break;
									case JANUS_VIDEOCODEC_H264:
										kf = janus_h264_is_keyframe(payload, plen);
										break;
									case JANUS_VIDEOCODEC_AV1:
										kf = janus_av1_is_keyframe(payload, plen);
										break;
									case JANUS_VIDEOCODEC_H265:
										kf = janus_h265_is_keyframe(payload, plen);
										break;
									default:
										break;
								}
								if(kf) {
									/* New keyframe, start saving it */
									source->keyframe.temp_ts = ntohl(rtp->timestamp);
									JANUS_LOG(LOG_HUGE, "[%s] New keyframe received! ts=%"SCNu32"\n", name, source->keyframe.temp_ts);
									janus_mutex_lock(&source->keyframe.mutex);
									janus_streaming_rtp_relay_packet *pkt = g_malloc0(sizeof(janus_streaming_rtp_relay_packet));
									pkt->data = g_malloc(bytes);
									memcpy(pkt->data, buffer, bytes);
									pkt->data->ssrc = htons(1);
									pkt->data->type = mountpoint->codecs.video_pt;
									pkt->is_rtp = TRUE;
									pkt->is_video = TRUE;
									pkt->is_keyframe = TRUE;
									pkt->length = bytes;
									pkt->timestamp = source->keyframe.temp_ts;
									pkt->seq_number = ntohs(rtp->seq_number);
									source->keyframe.temp_keyframe = g_list_append(source->keyframe.temp_keyframe, pkt);
									janus_mutex_unlock(&source->keyframe.mutex);
								}
							}
						}
					}
					/* If paused, ignore this packet */
					if(!mountpoint->enabled && !source->vrc)
						continue;
					//~ JANUS_LOG(LOG_VERB, " ... parsed RTP packet (ssrc=%u, pt=%u, seq=%u, ts=%u)...\n",
						//~ ntohl(rtp->ssrc), rtp->type, ntohs(rtp->seq_number), ntohl(rtp->timestamp));
					/* Relay on all sessions */
					packet.data = rtp;
					packet.length = bytes;
					packet.is_rtp = TRUE;
					packet.is_video = TRUE;
					packet.is_keyframe = FALSE;
					packet.simulcast = source->simulcast;
					packet.substream = index;
					packet.codec = mountpoint->codecs.video_codec;
					packet.svc = FALSE;
					if(source->svc) {
						/* We're doing SVC: let's parse this packet to see which layers are there */
						int plen = 0;
						char *payload = janus_rtp_payload(buffer, bytes, &plen);
						if(payload) {
							gboolean found = FALSE;
							memset(&packet.svc_info, 0, sizeof(packet.svc_info));
							if(janus_vp9_parse_svc(payload, plen, &found, &packet.svc_info) == 0) {
								packet.svc = found;
							}
						}
					}
					packet.data->type = mountpoint->codecs.video_pt;
					/* Is there a recorder? (FIXME notice we only record the first substream, if simulcasting) */
					janus_rtp_header_update(packet.data, &source->context[index], TRUE, 0);
					if(source->vskew) {
						int ret = janus_rtp_skew_compensate_video(packet.data, &source->context[index], now);
						if(ret < 0) {
							JANUS_LOG(LOG_WARN, "[%s] Dropping %d packets, video source clock is too fast (ssrc=%"SCNu32", index %d)\n",
								name, -ret, v_last_ssrc[index], index);
							continue;
						} else if(ret > 0) {
							JANUS_LOG(LOG_WARN, "[%s] Jumping %d RTP sequence numbers, video source clock is too slow (ssrc=%"SCNu32", index %d)\n",
								name, ret, v_last_ssrc[index], index);
						}
					}
					if(index == 0 && source->vrc) {
						packet.data->ssrc = htonl((uint32_t)mountpoint->id);
						janus_recorder_save_frame(source->vrc, buffer, bytes);
					}
					if (mountpoint->enabled) {
						packet.data->ssrc = htonl(ssrc);
						/* Backup the actual timestamp and sequence number set by the restreamer, in case switching is involved */
						packet.timestamp = ntohl(packet.data->timestamp);
						packet.seq_number = ntohs(packet.data->seq_number);
						/* Take note of the simulcast SSRCs */
						if(source->simulcast) {
							packet.ssrc[0] = v_last_ssrc[0];
							packet.ssrc[1] = v_last_ssrc[1];
							packet.ssrc[2] = v_last_ssrc[2];
						}
						/* Go! */
						janus_mutex_lock(&mountpoint->mutex);
						g_list_foreach(mountpoint->helper_threads == 0 ? mountpoint->viewers : mountpoint->threads,
							mountpoint->helper_threads == 0 ? janus_streaming_relay_rtp_packet : janus_streaming_helper_rtprtcp_packet,
							&packet);
						janus_mutex_unlock(&mountpoint->mutex);
					}
					continue;
				} else if(data_fd != -1 && fds[i].fd == data_fd) {
					/* Got something data (text) */
					if(mountpoint->active == FALSE)
						mountpoint->active = TRUE;
					source->last_received_data = janus_get_monotonic_time();
#ifdef HAVE_LIBCURL
					source->reconnect_timer = janus_get_monotonic_time();
#endif
					addrlen = sizeof(remote);
					bytes = recvfrom(data_fd, buffer, 1500, 0, (struct sockaddr *)&remote, &addrlen);
					if(bytes < 1) {
						/* Failed to read? */
						continue;
					}
					if(!mountpoint->enabled && !source->drc)
						continue;
					/* Copy the data */
					char *data = g_malloc(bytes);
					memcpy(data, buffer, bytes);
					/* Relay on all sessions */
					packet.data = (janus_rtp_header *)data;
					packet.length = bytes;
					packet.is_rtp = FALSE;
					packet.textdata = source->textdata;
					/* Is there a recorder? */
					janus_recorder_save_frame(source->drc, data, bytes);
					if(mountpoint->enabled) {
						/* Are we keeping track of the last message being relayed? */
						if(source->buffermsg) {
							janus_mutex_lock(&source->buffermsg_mutex);
							janus_streaming_rtp_relay_packet *pkt = g_malloc0(sizeof(janus_streaming_rtp_relay_packet));
							pkt->data = g_malloc(bytes);
							memcpy(pkt->data, data, bytes);
							packet.is_rtp = FALSE;
							pkt->length = bytes;
							janus_mutex_unlock(&source->buffermsg_mutex);
						}
						/* Go! */
						janus_mutex_lock(&mountpoint->mutex);
						g_list_foreach(mountpoint->helper_threads == 0 ? mountpoint->viewers : mountpoint->threads,
							mountpoint->helper_threads == 0 ? janus_streaming_relay_rtp_packet : janus_streaming_helper_rtprtcp_packet,
							&packet);
						janus_mutex_unlock(&mountpoint->mutex);
					}
					g_free(packet.data);
					packet.data = NULL;
					continue;
				} else if(audio_rtcp_fd != -1 && fds[i].fd == audio_rtcp_fd) {
					addrlen = sizeof(remote);
					bytes = recvfrom(audio_rtcp_fd, buffer, 1500, 0, (struct sockaddr *)&remote, &addrlen);
					if(bytes < 0 || (!janus_is_rtp(buffer, bytes) && !janus_is_rtcp(buffer, bytes))) {
						/* For latching we need an RTP or RTCP packet */
						continue;
					}
					if(!mountpoint->enabled)
						continue;
					memcpy(&source->audio_rtcp_addr, &remote, addrlen);
					if(!janus_is_rtcp(buffer, bytes)) {
						/* Failed to read or not an RTCP packet? */
						continue;
					}
					JANUS_LOG(LOG_HUGE, "[%s] Got audio RTCP feedback: SSRC %"SCNu32"\n",
						name, janus_rtcp_get_sender_ssrc(buffer, bytes));
					/* Relay on all sessions */
					packet.is_video = FALSE;
					packet.data = (janus_rtp_header *)buffer;
					packet.length = bytes;
					/* Go! */
					janus_mutex_lock(&mountpoint->mutex);
					g_list_foreach(mountpoint->helper_threads == 0 ? mountpoint->viewers : mountpoint->threads,
						mountpoint->helper_threads == 0 ? janus_streaming_relay_rtcp_packet : janus_streaming_helper_rtprtcp_packet,
						&packet);
					janus_mutex_unlock(&mountpoint->mutex);
				} else if(video_rtcp_fd != -1 && fds[i].fd == video_rtcp_fd) {
					addrlen = sizeof(remote);
					bytes = recvfrom(video_rtcp_fd, buffer, 1500, 0, (struct sockaddr *)&remote, &addrlen);
					if(bytes < 0 || (!janus_is_rtp(buffer, bytes) && !janus_is_rtcp(buffer, bytes))) {
						/* For latching we need an RTP or RTCP packet */
						continue;
					}
					if(!mountpoint->enabled)
						continue;
					memcpy(&source->video_rtcp_addr, &remote, addrlen);
					if(!janus_is_rtcp(buffer, bytes)) {
						/* Failed to read or not an RTCP packet? */
						continue;
					}
					JANUS_LOG(LOG_HUGE, "[%s] Got video RTCP feedback: SSRC %"SCNu32"\n",
						name, janus_rtcp_get_sender_ssrc(buffer, bytes));
					/* Relay on all sessions */
					packet.is_video = TRUE;
					packet.data = (janus_rtp_header *)buffer;
					packet.length = bytes;
					/* Go! */
					janus_mutex_lock(&mountpoint->mutex);
					g_list_foreach(mountpoint->helper_threads == 0 ? mountpoint->viewers : mountpoint->threads,
						mountpoint->helper_threads == 0 ? janus_streaming_relay_rtcp_packet : janus_streaming_helper_rtprtcp_packet,
						&packet);
					janus_mutex_unlock(&mountpoint->mutex);
				}
			}
		}
	}

	/* Notify users this mountpoint is done */
	janus_mutex_lock(&mountpoint->mutex);
	GList *viewer = g_list_first(mountpoint->viewers);
	/* Prepare JSON event */
	json_t *event = json_object();
	json_object_set_new(event, "streaming", json_string("event"));
	json_t *result = json_object();
	json_object_set_new(result, "status", json_string("stopped"));
	json_object_set_new(event, "result", result);
	while(viewer) {
		janus_streaming_session *session = (janus_streaming_session *)viewer->data;
		if(session == NULL) {
			mountpoint->viewers = g_list_remove_all(mountpoint->viewers, session);
			viewer = g_list_first(mountpoint->viewers);
			continue;
		}
		janus_mutex_lock(&session->mutex);
		if(session->mountpoint != mountpoint) {
			mountpoint->viewers = g_list_remove_all(mountpoint->viewers, session);
			viewer = g_list_first(mountpoint->viewers);
			janus_mutex_unlock(&session->mutex);
			continue;
		}
		g_atomic_int_set(&session->stopping, 1);
		g_atomic_int_set(&session->started, 0);
		g_atomic_int_set(&session->paused, 0);
		session->mountpoint = NULL;
		/* Tell the core to tear down the PeerConnection, hangup_media will do the rest */
		gateway->push_event(session->handle, &janus_streaming_plugin, NULL, event, NULL);
		gateway->close_pc(session->handle);
		janus_refcount_decrease(&session->ref);
		janus_refcount_decrease(&mountpoint->ref);
		mountpoint->viewers = g_list_remove_all(mountpoint->viewers, session);
		viewer = g_list_first(mountpoint->viewers);
		janus_mutex_unlock(&session->mutex);
	}
	json_decref(event);
	janus_mutex_unlock(&mountpoint->mutex);

	/* Unref the helper threads */
	if(mountpoint->helper_threads > 0) {
		GList *l = mountpoint->threads;
		while(l) {
			janus_streaming_helper *ht = (janus_streaming_helper *)l->data;
			janus_refcount_decrease(&ht->ref);
			l = l->next;
		}
	}

	JANUS_LOG(LOG_VERB, "[%s] Leaving streaming relay thread\n", name);
	g_free(name);
	janus_refcount_decrease(&mountpoint->ref);
	return NULL;
}

static void janus_streaming_relay_rtp_packet(gpointer data, gpointer user_data) {
	janus_streaming_rtp_relay_packet *packet = (janus_streaming_rtp_relay_packet *)user_data;
	if(!packet || !packet->data || packet->length < 1) {
		JANUS_LOG(LOG_ERR, "Invalid packet...\n");
		return;
	}
	janus_streaming_session *session = (janus_streaming_session *)data;
	if(!session || !session->handle) {
		//~ JANUS_LOG(LOG_ERR, "Invalid session...\n");
		return;
	}
	if(!packet->is_keyframe && (!g_atomic_int_get(&session->started) || g_atomic_int_get(&session->paused))) {
		//~ JANUS_LOG(LOG_ERR, "Streaming not started yet for this session...\n");
		return;
	}

	if(packet->is_rtp) {
		/* Make sure there hasn't been a video source switch by checking the SSRC */
		if(packet->is_video) {
			if(!session->video)
				return;
			/* Check if there's any SVC info to take into account */
			if(packet->svc) {
				/* There is: check if this is a layer that can be dropped for this viewer
				 * Note: Following core inspired by the excellent job done by Sergio Garcia Murillo here:
				 * https://github.com/medooze/media-server/blob/master/src/vp9/VP9LayerSelector.cpp */
				int plen = 0;
				char *payload = janus_rtp_payload((char *)packet->data, packet->length, &plen);
				gboolean keyframe = janus_vp9_is_keyframe((const char *)payload, plen);
				gboolean override_mark_bit = FALSE, has_marker_bit = packet->data->markerbit;
				int spatial_layer = session->spatial_layer;
				gint64 now = janus_get_monotonic_time();
				if(packet->svc_info.spatial_layer >= 0 && packet->svc_info.spatial_layer <= 2)
					session->last_spatial_layer[packet->svc_info.spatial_layer] = now;
				if(session->target_spatial_layer > session->spatial_layer) {
					JANUS_LOG(LOG_HUGE, "We need to upscale spatially: (%d < %d)\n",
						session->spatial_layer, session->target_spatial_layer);
					/* We need to upscale: wait for a keyframe */
					if(keyframe) {
						int new_spatial_layer = session->target_spatial_layer;
						while(new_spatial_layer > session->spatial_layer && new_spatial_layer > 0) {
							if(now - session->last_spatial_layer[new_spatial_layer] >= 250000) {
								/* We haven't received packets from this layer for a while, try a lower layer */
								JANUS_LOG(LOG_HUGE, "Haven't received packets from layer %d for a while, trying %d instead...\n",
									new_spatial_layer, new_spatial_layer-1);
								new_spatial_layer--;
							} else {
								break;
							}
						}
						if(new_spatial_layer > session->spatial_layer) {
							JANUS_LOG(LOG_HUGE, "  -- Upscaling spatial layer: %d --> %d (need %d)\n",
								session->spatial_layer, new_spatial_layer, session->target_spatial_layer);
							session->spatial_layer = new_spatial_layer;
							spatial_layer = session->spatial_layer;
							/* Notify the viewer */
							json_t *event = json_object();
							json_object_set_new(event, "streaming", json_string("event"));
							json_t *result = json_object();
							json_object_set_new(result, "spatial_layer", json_integer(session->spatial_layer));
							if(session->temporal_layer == -1) {
								/* We just started: initialize the temporal layer and notify that too */
								session->temporal_layer = 0;
								json_object_set_new(result, "temporal_layer", json_integer(session->temporal_layer));
							}
							json_object_set_new(event, "result", result);
							gateway->push_event(session->handle, &janus_streaming_plugin, NULL, event, NULL);
							json_decref(event);
						}
					}
				} else if(session->target_spatial_layer < session->spatial_layer) {
					/* We need to downscale */
					JANUS_LOG(LOG_HUGE, "We need to downscale spatially: (%d > %d)\n",
						session->spatial_layer, session->target_spatial_layer);
					gboolean downscaled = FALSE;
					if(!packet->svc_info.fbit && keyframe) {
						/* Non-flexible mode: wait for a keyframe */
						downscaled = TRUE;
					} else if(packet->svc_info.fbit && packet->svc_info.ebit) {
						/* Flexible mode: check the E bit */
						downscaled = TRUE;
					}
					if(downscaled) {
						JANUS_LOG(LOG_HUGE, "  -- Downscaling spatial layer: %d --> %d\n",
							session->spatial_layer, session->target_spatial_layer);
						session->spatial_layer = session->target_spatial_layer;
						/* Notify the viewer */
						json_t *event = json_object();
						json_object_set_new(event, "streaming", json_string("event"));
						json_t *result = json_object();
						json_object_set_new(result, "spatial_layer", json_integer(session->spatial_layer));
						json_object_set_new(event, "result", result);
						gateway->push_event(session->handle, &janus_streaming_plugin, NULL, event, NULL);
						json_decref(event);
					}
				}
				if(spatial_layer < packet->svc_info.spatial_layer) {
					/* Drop the packet: update the context to make sure sequence number is increased normally later */
					JANUS_LOG(LOG_HUGE, "Dropping packet (spatial layer %d < %d)\n", spatial_layer, packet->svc_info.spatial_layer);
					session->context.v_base_seq++;
					return;
				} else if(packet->svc_info.ebit && spatial_layer == packet->svc_info.spatial_layer) {
					/* If we stop at layer 0, we need a marker bit now, as the one from layer 1 will not be received */
					override_mark_bit = TRUE;
				}
				int temporal_layer = session->temporal_layer;
				if(session->target_temporal_layer > session->temporal_layer) {
					/* We need to upscale */
					JANUS_LOG(LOG_HUGE, "We need to upscale temporally: (%d < %d)\n",
						session->temporal_layer, session->target_temporal_layer);
					if(packet->svc_info.ubit && packet->svc_info.bbit &&
							packet->svc_info.temporal_layer > session->temporal_layer &&
							packet->svc_info.temporal_layer <= session->target_temporal_layer) {
						JANUS_LOG(LOG_HUGE, "  -- Upscaling temporal layer: %d --> %d (want %d)\n",
							session->temporal_layer, packet->svc_info.temporal_layer, session->target_temporal_layer);
						session->temporal_layer = packet->svc_info.temporal_layer;
						temporal_layer = session->temporal_layer;
						/* Notify the viewer */
						json_t *event = json_object();
						json_object_set_new(event, "streaming", json_string("event"));
						json_t *result = json_object();
						json_object_set_new(result, "temporal_layer", json_integer(session->temporal_layer));
						json_object_set_new(event, "result", result);
						gateway->push_event(session->handle, &janus_streaming_plugin, NULL, event, NULL);
						json_decref(event);
					}
				} else if(session->target_temporal_layer < session->temporal_layer) {
					/* We need to downscale */
					JANUS_LOG(LOG_HUGE, "We need to downscale temporally: (%d > %d)\n",
						session->temporal_layer, session->target_temporal_layer);
					if(packet->svc_info.ebit && packet->svc_info.temporal_layer == session->target_temporal_layer) {
						JANUS_LOG(LOG_HUGE, "  -- Downscaling temporal layer: %d --> %d\n",
							session->temporal_layer, session->target_temporal_layer);
						session->temporal_layer = session->target_temporal_layer;
						/* Notify the viewer */
						json_t *event = json_object();
						json_object_set_new(event, "streaming", json_string("event"));
						json_t *result = json_object();
						json_object_set_new(result, "temporal_layer", json_integer(session->temporal_layer));
						json_object_set_new(event, "result", result);
						gateway->push_event(session->handle, &janus_streaming_plugin, NULL, event, NULL);
						json_decref(event);
					}
				}
				if(temporal_layer < packet->svc_info.temporal_layer) {
					/* Drop the packet: update the context to make sure sequence number is increased normally later */
					JANUS_LOG(LOG_HUGE, "Dropping packet (temporal layer %d < %d)\n", temporal_layer, packet->svc_info.temporal_layer);
					session->context.v_base_seq++;
					return;
				}
				/* If we got here, we can send the frame: this doesn't necessarily mean it's
				 * one of the layers the user wants, as there may be dependencies involved */
				JANUS_LOG(LOG_HUGE, "Sending packet (spatial=%d, temporal=%d)\n",
					packet->svc_info.spatial_layer, packet->svc_info.temporal_layer);
				/* Fix sequence number and timestamp (publisher switching may be involved) */
				janus_rtp_header_update(packet->data, &session->context, TRUE, 0);
				if(override_mark_bit && !has_marker_bit) {
					packet->data->markerbit = 1;
				}
				janus_plugin_rtp rtp = { .video = packet->is_video, .buffer = (char *)packet->data, .length = packet->length };
				janus_plugin_rtp_extensions_reset(&rtp.extensions);
				if(gateway != NULL)
					gateway->relay_rtp(session->handle, &rtp);
				if(override_mark_bit && !has_marker_bit) {
					packet->data->markerbit = 0;
				}
				/* Restore the timestamp and sequence number to what the publisher set them to */
				packet->data->timestamp = htonl(packet->timestamp);
				packet->data->seq_number = htons(packet->seq_number);
			} else if(packet->simulcast) {
				/* Handle simulcast: don't relay if it's not the substream we wanted to handle */
				int plen = 0;
				char *payload = janus_rtp_payload((char *)packet->data, packet->length, &plen);
				if(payload == NULL)
					return;
				/* Process this packet: don't relay if it's not the SSRC/layer we wanted to handle */
				gboolean relay = janus_rtp_simulcasting_context_process_rtp(&session->sim_context,
					(char *)packet->data, packet->length, packet->ssrc, NULL, packet->codec, &session->context);
				if(session->sim_context.need_pli) {
					/* Schedule a PLI */
					JANUS_LOG(LOG_VERB, "We need a PLI for the simulcast context\n");
					if(session->mountpoint != NULL) {
						janus_streaming_rtp_source *source = session->mountpoint->source;
						if(source != NULL)
							g_atomic_int_set(&source->need_pli, 1);
					}
				}
				/* Do we need to drop this? */
				if(!relay)
					return;
				/* Any event we should notify? */
				if(session->sim_context.changed_substream) {
					/* Notify the user about the substream change */
					json_t *event = json_object();
					json_object_set_new(event, "streaming", json_string("event"));
					json_t *result = json_object();
					json_object_set_new(result, "substream", json_integer(session->sim_context.substream));
					json_object_set_new(event, "result", result);
					gateway->push_event(session->handle, &janus_streaming_plugin, NULL, event, NULL);
					json_decref(event);
				}
				if(session->sim_context.changed_temporal) {
					/* Notify the user about the temporal layer change */
					json_t *event = json_object();
					json_object_set_new(event, "streaming", json_string("event"));
					json_t *result = json_object();
					json_object_set_new(result, "temporal", json_integer(session->sim_context.templayer));
					json_object_set_new(event, "result", result);
					gateway->push_event(session->handle, &janus_streaming_plugin, NULL, event, NULL);
					json_decref(event);
				}
				/* If we got here, update the RTP header and send the packet */
				janus_rtp_header_update(packet->data, &session->context, TRUE, 0);
				char vp8pd[6];
				if(packet->codec == JANUS_VIDEOCODEC_VP8) {
					/* For VP8, we save the original payload descriptor, to restore it after */
					memcpy(vp8pd, payload, sizeof(vp8pd));
					janus_vp8_simulcast_descriptor_update(payload, plen, &session->vp8_context,
						session->sim_context.changed_substream);
				}
				/* Send the packet */
				janus_plugin_rtp rtp = { .video = packet->is_video, .buffer = (char *)packet->data, .length = packet->length };
				janus_plugin_rtp_extensions_reset(&rtp.extensions);
				if(gateway != NULL)
					gateway->relay_rtp(session->handle, &rtp);
				/* Restore the timestamp and sequence number to what the publisher set them to */
				packet->data->timestamp = htonl(packet->timestamp);
				packet->data->seq_number = htons(packet->seq_number);
				if(packet->codec == JANUS_VIDEOCODEC_VP8) {
					/* Restore the original payload descriptor as well, as it will be needed by the next viewer */
					memcpy(payload, vp8pd, sizeof(vp8pd));
				}
			} else {
				/* Fix sequence number and timestamp (switching may be involved) */
				janus_rtp_header_update(packet->data, &session->context, TRUE, 0);
				janus_plugin_rtp rtp = { .video = packet->is_video, .buffer = (char *)packet->data, .length = packet->length };
				janus_plugin_rtp_extensions_reset(&rtp.extensions);
				if(gateway != NULL)
					gateway->relay_rtp(session->handle, &rtp);
				/* Restore the timestamp and sequence number to what the video source set them to */
				packet->data->timestamp = htonl(packet->timestamp);
				packet->data->seq_number = htons(packet->seq_number);
			}
		} else {
			if(!session->audio)
				return;
			/* Fix sequence number and timestamp (switching may be involved) */
			janus_rtp_header_update(packet->data, &session->context, FALSE, 0);
			janus_plugin_rtp rtp = { .video = packet->is_video, .buffer = (char *)packet->data, .length = packet->length };
			janus_plugin_rtp_extensions_reset(&rtp.extensions);
			if(gateway != NULL)
				gateway->relay_rtp(session->handle, &rtp);
			/* Restore the timestamp and sequence number to what the video source set them to */
			packet->data->timestamp = htonl(packet->timestamp);
			packet->data->seq_number = htons(packet->seq_number);
		}
	} else {
		/* We're broadcasting a data channel message */
		if(!session->data)
			return;
		if(gateway != NULL && packet->data != NULL && g_atomic_int_get(&session->dataready)) {
			janus_plugin_data data = {
				.label = NULL,
				.protocol = NULL,
				.binary = !packet->textdata,
				.buffer = (char *)packet->data,
				.length = packet->length
			};
			gateway->relay_data(session->handle, &data);
		}
	}

	return;
}


static void janus_streaming_relay_rtcp_packet(gpointer data, gpointer user_data) {
	janus_streaming_rtp_relay_packet *packet = (janus_streaming_rtp_relay_packet *)user_data;
	if(!packet || !packet->data || packet->length < 1) {
		JANUS_LOG(LOG_ERR, "Invalid packet...\n");
		return;
	}
	janus_streaming_session *session = (janus_streaming_session *)data;
	if(!session || !session->handle) {
		//~ JANUS_LOG(LOG_ERR, "Invalid session...\n");
		return;
	}
	if(!g_atomic_int_get(&session->started) || g_atomic_int_get(&session->paused)) {
		//~ JANUS_LOG(LOG_ERR, "Streaming not started yet for this session...\n");
		return;
	}

	janus_plugin_rtcp rtcp = { .video = packet->is_video, .buffer = (char *)packet->data, .length = packet->length };
	if(gateway != NULL)
		gateway->relay_rtcp(session->handle, &rtcp);

	return;
}

static void janus_streaming_helper_rtprtcp_packet(gpointer data, gpointer user_data) {
	janus_streaming_rtp_relay_packet *packet = (janus_streaming_rtp_relay_packet *)user_data;
	if(!packet || !packet->data || packet->length < 1) {
		JANUS_LOG(LOG_ERR, "Invalid packet...\n");
		return;
	}
	janus_streaming_helper *helper = (janus_streaming_helper *)data;
	if(!helper) {
		//~ JANUS_LOG(LOG_ERR, "Invalid session...\n");
		return;
	}
	/* Clone the packet and queue it for delivery on the helper thread */
	janus_streaming_rtp_relay_packet *copy = g_malloc0(sizeof(janus_streaming_rtp_relay_packet));
	copy->data = g_malloc(packet->length);
	memcpy(copy->data, packet->data, packet->length);
	copy->length = packet->length;
	copy->is_rtp = packet->is_rtp;
	copy->is_video = packet->is_video;
	copy->is_keyframe = packet->is_keyframe;
	copy->simulcast = packet->simulcast;
	copy->ssrc[0] = packet->ssrc[0];
	copy->ssrc[1] = packet->ssrc[1];
	copy->ssrc[2] = packet->ssrc[2];
	copy->codec = packet->codec;
	copy->substream = packet->substream;
	copy->timestamp = packet->timestamp;
	copy->seq_number = packet->seq_number;
	g_async_queue_push(helper->queued_packets, copy);
}

static void *janus_streaming_helper_thread(void *data) {
	janus_streaming_helper *helper = (janus_streaming_helper *)data;
	janus_streaming_mountpoint *mp = helper->mp;
	JANUS_LOG(LOG_INFO, "[%s/#%d] Joining Streaming helper thread\n", mp->name, helper->id);
	janus_streaming_rtp_relay_packet *pkt = NULL;
	while(!g_atomic_int_get(&stopping) && !g_atomic_int_get(&mp->destroyed) && !g_atomic_int_get(&helper->destroyed)) {
		pkt = g_async_queue_pop(helper->queued_packets);
		if(pkt == &exit_packet)
			break;
		janus_mutex_lock(&helper->mutex);
		g_list_foreach(helper->viewers,
			pkt->is_rtp ? janus_streaming_relay_rtp_packet : janus_streaming_relay_rtcp_packet,
			pkt);
		janus_mutex_unlock(&helper->mutex);
		janus_streaming_rtp_relay_packet_free(pkt);
	}
	JANUS_LOG(LOG_INFO, "[%s/#%d] Leaving Streaming helper thread\n", mp->name, helper->id);
	janus_refcount_decrease(&helper->ref);
	janus_refcount_decrease(&mp->ref);
	return NULL;
}
