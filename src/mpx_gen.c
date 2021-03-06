/*
 * mpxgen - FM multiplex encoder with Stereo and RDS
 * Copyright (C) 2019 Anthony96922
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <signal.h>
#include <getopt.h>
#include <string.h>
#include <ao/ao.h>

#include "rds.h"
#include "fm_mpx.h"
#include "control_pipe.h"
#include "cpu.h"

int stop_mpx;

void stop() {
	stop_mpx = 1;
}

void float2char(float *inbuf, char *outbuf, size_t inbufsize) {
	int j = 0;
	short sample;
	for (int i = 0; i < inbufsize; i++) {
		sample = inbuf[i] * 32767;
		outbuf[j+0] = sample & 255;
		outbuf[j+1] = sample >> 8;
		j += 2;
	}
}

void float2char_2ch(float *inbuf, char *outbuf, size_t inbufsize) {
	int j = 0;
	short sample;
	for (int i = 0; i < inbufsize; i++) {
		sample = inbuf[i] * 32767;
		outbuf[j+0] = outbuf[j+2] = sample & 255;
		outbuf[j+1] = outbuf[j+3] = sample >> 8;
		j += 4;
	}
}

int generate_mpx(char *audio_file, char *output_file, char *control_pipe, float mpx, float ppm, int wait, int rds, uint16_t pi, char *ps, char *rt, int pty, int tp, int *af, char *ptyn) {
	// Gracefully stop the encoder on SIGINT or SIGTERM
	signal(SIGINT, stop);
	signal(SIGTERM, stop);

	// Work around random audio ticks
	set_affinity(3);

	// Data structures for baseband data
	float mpx_data[DATA_SIZE];
	float resample_out[DATA_SIZE];
	char dev_out[DATA_SIZE];

	// AO
	ao_device *device;
	ao_sample_format format;
	format.bits = 16;
	format.rate = 192000;
	format.byte_format = AO_FMT_LITTLE;

	ao_initialize();

	if (output_file != NULL) {
		format.channels = 1;
		if ((device = ao_open_file(ao_driver_id("wav"), output_file, 1, &format, NULL)) == NULL) {
			fprintf(stderr, "Error: cannot open output file.\n");
			return 1;
		}
	} else {
		format.channels = 2;
		if ((device = ao_open_live(ao_default_driver_id(), &format, NULL)) == NULL) {
			fprintf(stderr, "Error: cannot open sound device.\n");
			return 1;
		}
	}

	// SRC
	int src_error;

	SRC_STATE *src_state;
	SRC_DATA src_data;
	src_data.src_ratio = (192000 / 228000.0) + (ppm / 1000000);
	src_data.output_frames = DATA_SIZE;
	src_data.data_in = mpx_data;
	src_data.data_out = resample_out;

	if ((src_state = src_new(CONVERTER_TYPE, 1, &src_error)) == NULL) {
		fprintf(stderr, "Error: src_new failed: %s\n", src_strerror(src_error));
		return 1;
	}

	// Initialize the baseband generator
	if(fm_mpx_open(audio_file, wait) < 0) return 1;
	set_output_volume(mpx);

	// Initialize the RDS modulator
	set_rds_switch(rds);
	if (init_rds_encoder(pi, ps, rt, pty, tp, af, ptyn) < 0) return 1;

	// Initialize the control pipe reader
	if(control_pipe) {
		if(open_control_pipe(control_pipe) == 0) {
			printf("Reading control commands on %s.\n", control_pipe);
		} else {
			printf("Failed to open control pipe: %s.\n", control_pipe);
			control_pipe = NULL;
		}
	}

	for (;;) {
		if(control_pipe) poll_control_pipe();

		if ((src_data.input_frames = fm_mpx_get_samples(mpx_data)) < 0) break;

		if ((src_error = src_process(src_state, &src_data))) {
			fprintf(stderr, "Error: src_process failed: %s\n", src_strerror(src_error));
			break;
		}

		if (format.channels == 2)
			float2char_2ch(resample_out, dev_out, src_data.output_frames_gen);
		else
			float2char(resample_out, dev_out, src_data.output_frames_gen);

		// num_bytes = src_data.output_frames_gen * channels * bytes per sample
		if (!ao_play(device, dev_out, src_data.output_frames_gen * format.channels * sizeof(short))) {
			fprintf(stderr, "Error: could not play audio.\n");
			break;
		}

		if (stop_mpx) {
			printf("Stopping...\n");
			break;
		}
	}

	close_control_pipe();
	fm_mpx_close();

	ao_close(device);
	ao_shutdown();

	src_delete(src_state);

	return 0;
}

int main(int argc, char **argv) {
	int opt;
	char *audio_file = NULL;
	char *output_file = NULL;
	char *control_pipe = NULL;
	int rds = 1;
	int alternative_freq[MAX_AF+1];
	int af_size = 0;
	// Use arrays to enforce max length for RDS text items
	char ps[9] = "mpxgen";
	char rt[65] = "mpxgen: FM Stereo and RDS encoder";
	char ptyn[9] = {0};
	uint16_t pi = 0xFFFF;
	int pty = 0;
	int tp = 0;
	float ppm = 0;
	float mpx = 50;
	int wait = 1;

	const char	*short_opt = "a:o:m:x:W:R:i:s:r:p:T:A:P:C:h";
	struct option	long_opt[] =
	{
		{"audio", 	required_argument, NULL, 'a'},
		{"output-file",	required_argument, NULL, 'o'},
		{"mpx",		required_argument, NULL, 'm'},
		{"ppm",		required_argument, NULL, 'x'},
		{"wait",	required_argument, NULL, 'W'},

		{"rds", 	required_argument, NULL, 'R'},
		{"pi",		required_argument, NULL, 'i'},
		{"ps",		required_argument, NULL, 's'},
		{"rt",		required_argument, NULL, 'r'},
		{"pty",		required_argument, NULL, 'p'},
		{"tp",		required_argument, NULL, 'T'},
		{"af",		required_argument, NULL, 'A'},
		{"ptyn",	required_argument, NULL, 'P'},
		{"ctl",		required_argument, NULL, 'C'},

		{"help",	no_argument, NULL, 'h'},
		{ 0,		0,		0,	0 }
	};

	while((opt = getopt_long(argc, argv, short_opt, long_opt, NULL)) != -1)
	{
		switch(opt)
		{
			case 'a': //audio
				audio_file = optarg;
				break;

			case 'o': //output-file
				output_file = optarg;
				break;

			case 'm': //mpx
				mpx = atoi(optarg);
				if (mpx < 1 || mpx > 100) {
					fprintf(stderr, "MPX volume must be between 1 - 100.\n");
					return 1;
				}
				break;

			case 'x': //ppm
				ppm = atof(optarg);
				break;

			case 'W': //wait
				wait = atoi(optarg);
				break;

			case 'R': //rds
				rds = atoi(optarg);
				break;

			case 'i': //pi
				pi = (uint16_t) strtol(optarg, NULL, 16);
				break;

			case 's': //ps
				strncpy(ps, optarg, 8);
				break;

			case 'r': //rt
				strncpy(rt, optarg, 64);
				break;

			case 'p': //pty
				pty = atoi(optarg);
				break;

			case 'T': //tp
				tp = atoi(optarg);
				break;

			case 'A': //af
				af_size++;
				if (af_size > MAX_AF) {
					fprintf(stderr, "AF list is too large.\n");
					return 1;
				}
				alternative_freq[af_size] = (int)(10*atof(optarg))-875;
				if(alternative_freq[af_size] < 1 || alternative_freq[af_size] > 204) {
					fprintf(stderr, "Alternative Frequency has to be set in range of 87.6 MHz - 107.9 MHz\n");
					return 1;
				}
				break;

			case 'P': //ptyn
				strncpy(ptyn, optarg, 8);
				break;

			case 'C': //ctl
				control_pipe = optarg;
				break;

			case 'h': //help
				fprintf(stderr, "Help: %s\n"
				      "	[--audio (-a) file]\n"
				      "	[--output-file (-o) WAVE out]\n"
				      "	[--ppm (-x) clock correction]\n"
				      "	[--mpx (-m) mpx-volume]\n"
				      "	[--wait (-W) wait-switch]\n"
				      "	[--rds (-R) rds-switch]\n"
				      "	[--pi (-i) pi-code]\n"
				      "	[--ps (-s) ps-text]\n"
				      "	[--rt (-r) radiotext]\n"
				      "	[--pty (-p) program-type]\n"
				      "	[--tp (-T) traffic-program]\n"
				      "	[--af (-A) alternative-freq]\n"
				      "	[--ptyn (-P) pty-name]\n"
				      "	[--ctl (-C) control-pipe]\n", argv[0]);
				return 1;

			default:
				fprintf(stderr, "(See -h / --help)\n");
				return 1;
		}
	}

	if (audio_file == NULL && !rds) {
		fprintf(stderr, "Nothing to do. Exiting.\n");
		return 1;
	}

	alternative_freq[0] = af_size;

	return generate_mpx(audio_file, output_file, control_pipe, mpx, ppm, wait, rds, pi, ps, rt, pty, tp, alternative_freq, ptyn);
}
