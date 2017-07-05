/* Network Capture
 *
 * Creates a file per stream and writes the result to disk. These files can
 * later be merged using tracemerge to create a single ordered trace file.
 *
 * An alternative approach if we want a single output file is to use the
 * ordered combiner and publish the packets through to a reporter thread that
 * does the writing to disk.
 *
 * Defaults to using 4 threads, but this can be changed using the -t option.
 */
/* Note we include libtrace_parallel.h rather then libtrace.h */
#include "libtrace_parallel.h"
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <inttypes.h>
#include <signal.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <wandio.h>

static char *output = NULL;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int count = 0;
static libtrace_t *trace = NULL;

static int compress_level=-1;
static trace_option_compresstype_t compress_type = TRACE_OPTION_COMPRESSTYPE_NONE;

static void stop(int signal UNUSED)
{
	if (trace)
		trace_pstop(trace);
}

static void usage(char *argv0)
{
	fprintf(stderr,"Usage:\n"
	"%s [options] inputfile outputfile\n"
	"-t --threads	The number of threads to use\n"
	"-S --snaplen	The snap length\n"
	"-H --libtrace-help	Print libtrace runtime documentation\n"
	"-z --compress-level	Compress the output trace at the specified level\n"
	"-Z --compress-type 	Compress the output trace using the specified"
	"			compression algorithm\n"
	,argv0);
	exit(1);
}

/* Creates an output trace and configures it according to our preferences */
static libtrace_out_t *create_output(int my_id) {
	libtrace_out_t *out = NULL;
	char name[1024];
	const char * file_index = NULL;
	const char * first_extension = NULL;

	file_index = strrchr(output, '/');
	if (file_index)
		first_extension = strchr(file_index, '.');
	else
		first_extension = strchr(name, '.');

	if (first_extension) {
		snprintf(name, sizeof(name), "%.*s-%d%s", (int) (first_extension - output), output, my_id, first_extension);
	} else {
		snprintf(name, sizeof(name), "%s-%d", output, my_id);
	}

	out = trace_create_output(name);
	assert(out);

	if (compress_level >= 0 && trace_config_output(out,
			TRACE_OPTION_OUTPUT_COMPRESS, &compress_level) == -1) {
		trace_perror_output(out, "Configuring compression level");
		trace_destroy_output(out);
		exit(-1);
	}

	if (trace_config_output(out, TRACE_OPTION_OUTPUT_COMPRESSTYPE,
				&compress_type) == -1) {
		trace_perror_output(out, "Configuring compression type");
		trace_destroy_output(out);
		exit(-1);
	}

	if (trace_start_output(out)==-1) {
		trace_perror_output(out,"trace_start_output");
		trace_destroy_output(out);
		exit(-1);
	}
	return out;
}


static libtrace_packet_t *per_packet(libtrace_t *trace UNUSED,
                libtrace_thread_t *t UNUSED,
                void *global UNUSED, void *tls, libtrace_packet_t *packet) {

        /* Retrieve our output trace from the thread local storage */
        libtrace_out_t *output = (libtrace_out_t *)tls;

        /* Write the packet to disk */
        trace_write_packet(output, packet);

        /* Return the packet as we are finished with it */
        return packet;

}

/* Creates an output file for this thread */
static void *init_process(libtrace_t *trace UNUSED, libtrace_thread_t *t UNUSED,
                void *global UNUSED) {

        int my_id = 0;
        libtrace_out_t *out;

        pthread_mutex_lock(&lock);
        my_id = ++count;
        pthread_mutex_unlock(&lock);
        out = create_output(my_id);

        return out;
}

/* Closes the output file for this thread */
static void stop_process(libtrace_t *trace UNUSED, libtrace_thread_t *t UNUSED,
                void *global UNUSED, void *tls) {

        /* Retrieve our output trace from the thread local storage */
        libtrace_out_t *output = (libtrace_out_t *)tls;

        trace_destroy_output(output);
}

int main(int argc, char *argv[])
{
	struct sigaction sigact;
	libtrace_stat_t *stats;
	int snaplen = -1;
	int nb_threads = 4;     
	char *compress_type_str=NULL;
        libtrace_callback_set_t *processing = NULL;

	sigact.sa_handler = stop;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = SA_RESTART;

	sigaction(SIGINT, &sigact, NULL);


	if (argc<3) {
		usage(argv[0]);
		return 1;
	}

	/* Parse command line options */
	while(1) {
		int option_index;
		struct option long_options[] = {
			{ "libtrace-help", 0, 0, 'H' },
			{ "threads",	   1, 0, 't' },
			{ "snaplen",	   1, 0, 'S' },
			{ "compress-level", 1, 0, 'z' },
			{ "compress-type", 1, 0, 'Z' },
			{ NULL, 	   0, 0, 0   },
		};

		int c=getopt_long(argc, argv, "t:S:Hz:Z:",
		                  long_options, &option_index);

		if (c==-1)
			break;

		switch (c) {
		case  't': nb_threads = atoi(optarg);
			break;
		case 'S': snaplen=atoi(optarg);
			break;
		case 'H':
			trace_help();
			exit(1);
			break;
		case 'z':
			compress_level=atoi(optarg);
			if (compress_level<0 || compress_level>9) {
				usage(argv[0]);
				exit(1);
			}
			break;
		case 'Z':
			compress_type_str=optarg;
			break;
		default:
			fprintf(stderr,"Unknown option: %c\n",c);
			usage(argv[0]);
			return 1;
		}
	}

	if (compress_type_str == NULL && compress_level >= 0) {
		fprintf(stderr, "Compression level set, but no compression type was defined, setting to gzip\n");
		compress_type = TRACE_OPTION_COMPRESSTYPE_ZLIB;
	}

	else if (compress_type_str == NULL) {
		/* If a level or type is not specified, use the "none"
		 * compression module */
		compress_type = TRACE_OPTION_COMPRESSTYPE_NONE;
	}

	else {
	    struct wandio_compression_type* compression_type = wandio_lookup_compression_type(compress_type_str);
	    if (compression_type == NULL) {
		fprintf(stderr, "Unknown compression type: %s\n",
		        compress_type_str);
		return 1;
	}
	    compress_type = compression_type->compress_type;
	}


	if (argc-optind != 2) {
		usage(argv[0]);
		return 1;
	}
	trace = trace_create(argv[optind]);
	output = argv[optind+1];

	if (trace_is_err(trace)) {
		trace_perror(trace,"Opening trace file");
		return 1;
	}

        /* Set up our callbacks */
        processing = trace_create_callback_set();
        trace_set_starting_cb(processing, init_process);
        trace_set_stopping_cb(processing, stop_process);
        trace_set_packet_cb(processing, per_packet);

	if (snaplen != -1)
		trace_set_snaplen(trace, snaplen);
	if(nb_threads != -1)
		trace_set_perpkt_threads(trace, nb_threads);

	/* We use a new version of trace_start(), trace_pstart()
	 * The reporter function argument is optional and can be NULL */
	if (trace_pstart(trace, NULL, processing, NULL)) {
		trace_perror(trace,"Starting trace");
		trace_destroy(trace);
                trace_destroy_callback_set(processing);
		return 1;
	}

	/* Wait for the trace to finish */
	trace_join(trace);

	if (trace_is_err(trace)) {
		trace_perror(trace,"Reading packets");
		trace_destroy(trace);
                trace_destroy_callback_set(processing);
		return 1;
	}

	/* Print stats before we destroy the trace */
	stats = trace_get_statistics(trace, NULL);
	fprintf(stderr, "Overall statistics\n");
	trace_print_statistics(stats, stderr, "\t%s: %"PRIu64"\n");

        trace_destroy_callback_set(processing);
	trace_destroy(trace);
	return 0;
}
