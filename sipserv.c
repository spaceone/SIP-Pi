/*
=================================================================================
 Name        : sipserv.c
 Version     : 0.1

 Copyright (C) 2012 by Andre Wussow, desk@binerry.de
               2017 by Fabian Huslik, github/fabianhu

 Description :
     Tool for automated, flexible answered calls over SIP/VOIP with PJSUA library and eSpeak.

 Dependencies:
	- PJSUA API (PJSIP)
	- eSpeak

 References  :
 http://www.pjsip.org/
 http://www.pjsip.org/docs/latest/pjsip/docs/html/group__PJSUA__LIB.htm
 http://espeak.sourceforge.net/
 http://binerry.de/post/29180946733/raspberry-pi-caller-and-answering-machine
 https://github.com/fabianhu/Sip-Pi

================================================================================
This tool is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This tool is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.
================================================================================
*/

// definition of endianess (e.g. needed on raspberry pi)
#define PJ_IS_LITTLE_ENDIAN 1
#define PJ_IS_BIG_ENDIAN 0

// includes
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <pjsua-lib/pjsua.h>

// some espeak options
#define ESPEAK_AMPLITUDE 100
#define ESPEAK_CAPITALS_PITCH 20
#define ESPEAK_SPEED 120
#define ESPEAK_PITCH 75

// disable pjsua logging
#define PJSUA_LOG_LEVEL 0

// define max supported dtmf settings
#define MAX_DTMF_SETTINGS 9

// struct for app dtmf settings
struct dtmf_config {
	int id;
	int active;
	int processing_active;
	char *description;
	char *tts_intro;
	char *tts_answer;
	char *cmd;
};

// struct for app configuration settings
struct app_config {
	char *sip_domain;
	char *sip_user;
	char *sip_password;
	char *language;
	int record_calls;
	int silent_mode;
	char *tts;
	char *announcement_file;
	char *CallCmd;
	char *AfterMath;
	char *log_file;
	struct dtmf_config dtmf_cfg[MAX_DTMF_SETTINGS];
} app_cfg;

// global holder vars for further app arguments
char *tts_file = "play.wav";
char *tts_answer_file = "ans.wav";
char rec_ans_file[100] = "rec.wav"; // will be overwritten!
char lastNumber[100] = "000"; // will be overwritten!

// global helper vars
int app_exiting = 0;

// global vars for pjsua
pjsua_acc_id acc_id;
pjsua_player_id play_id = PJSUA_INVALID_ID;
pjmedia_port *play_port;
pjsua_recorder_id rec_id = PJSUA_INVALID_ID;
pjsua_call_id current_call = PJSUA_INVALID_ID;

// header of helper-methods
static void create_player(pjsua_call_id, char *);
static void create_recorder(pjsua_call_info);
static void log_message(char *);
static void parse_config_file(char *);
static void register_sip(void);
static void setup_sip(void);
static int synthesize_speech(char *, char *, char *);
static void usage(int);
static int try_get_argument(int, char *, char **, int, char *[]);
static int callBash(char* command, char* result);


// header of callback-methods
static void on_incoming_call(pjsua_acc_id, pjsua_call_id, pjsip_rx_data *);
static void on_call_media_state(pjsua_call_id);
static void on_call_state(pjsua_call_id, pjsip_event *);
static void on_dtmf_digit(pjsua_call_id, int);
static void signal_handler(int);
static char *trim_string(char *);

// header of app-control-methods
static void app_exit();
static void error_exit(const char *, pj_status_t);

// main application
int main(int argc, char *argv[])
{
	// first set some default values
	app_cfg.record_calls = 0;
	app_cfg.silent_mode = 0;

	// print infos
	log_message("SIP Call - Simple TTS/DTMF-based answering machine\n");
	log_message("==================================================\n");

	// register signal handler for break-in-keys (e.g. ctrl+c)
	signal(SIGINT, signal_handler);
	signal(SIGKILL, signal_handler);

	// init dtmf settings (dtmf 0 is reserved for exit call)
	int i;
	for (i = 0; i < MAX_DTMF_SETTINGS; i++)
	{
		struct dtmf_config *d_cfg = &app_cfg.dtmf_cfg[i];
		d_cfg->id = i+1;
		d_cfg->active = 0;
		d_cfg->processing_active = 0;
	}

	// parse arguments
	if (argc > 1)
	{
		int arg;
		for( arg = 1; arg < argc; arg+=2 )
		{
			// check if usage info needs to be displayed
			if (!strcasecmp(argv[arg], "--help") || !strcasecmp(argv[arg], "-help") || !strcasecmp(argv[arg], "-h"))
			{
				// display usage info and exit app
				usage(0);
				exit(0);
			}

			// check for config file location
			if (!strcasecmp(argv[arg], "--config-file"))
			{
				if (argc >= (arg+1))
				{
					app_cfg.log_file = argv[arg+1];
				}
				continue;
			}

			// check for silent mode option
			char *s;
			try_get_argument(arg, "-s", &s, argc, argv);
			if (!strcasecmp(s, "1"))
			{
				app_cfg.silent_mode = 1;
				continue;
			}
		}
	}

	if (!app_cfg.log_file)
	{
		// too few arguments specified - display usage info and exit app
		usage(1);
		exit(1);
	}

	// read app configuration from config file
	parse_config_file(app_cfg.log_file);

	if (!app_cfg.sip_domain || !app_cfg.sip_user || !app_cfg.sip_password || !app_cfg.language)
	{
		log_message("Not enough stuff in config file\nsee sipserv -h\n");
		// display usage info and exit app
		usage(2); // fixme does not show after file has been opened.
		exit(1);
	}

	if	(app_cfg.announcement_file)
	{
		log_message("Announcement mode\n");

		FILE *file;
		if ((file = fopen(app_cfg.announcement_file, "r")) == NULL)
		{
			if (errno == ENOENT)
			{
				log_message("Announcement file doesn't exist");
			}
			else
			{
				// Check for other errors too, like EACCES and EISDIR
				log_message("Announcement file: some other error occured");
			}
			exit(1);
		}
		else
		{
			fclose(file);
		}
	}

	// generate texts
	log_message("Generating texts ... ");

	char tts_buffer[1024];
	strcpy(tts_buffer, app_cfg.tts);
	strcat(tts_buffer, " ");

	for (i = 0; i < MAX_DTMF_SETTINGS; i++)
	{
		struct dtmf_config *d_cfg = &app_cfg.dtmf_cfg[i];

		if (d_cfg->active == 1)
		{
			strcat(tts_buffer, d_cfg->tts_intro);
			strcat(tts_buffer, " ");
		}
	}

	log_message("Done.\n");

	// synthesizing speech
	log_message("Synthesizing speech ... ");

	int synth_status = -1;
	synth_status = synthesize_speech(tts_buffer, tts_file, app_cfg.language);
	if (synth_status != 0) error_exit("Error while creating phone text", synth_status);
	log_message("Done.\n");

	// setup up sip library pjsua
	setup_sip();

	// create account and register to sip server
	register_sip();

	// app loop
	for (;;) {
	    sleep(10); // avoid locking up the system
	}

	// exit app
	app_exit();

	return 0;
}

// helper for displaying usage infos
static void usage(int error)
{

	if (error == 1)
	{
		puts("Error, too few arguments.");
		puts  ("");
	}
	if (error == 2)
	{
		puts("Missing mandatory items in config file.");
		puts  ("");
	}
	puts  ("Usage:");
    puts  ("  sipserv [options]");
    puts  ("");
	puts  ("Commandline:");
    puts  ("Mandatory options:");
    puts  ("  --config-file=string   Set config file");
    puts  ("");
	puts  ("Optional options:");
	puts  ("  -s=int       Silent mode (hide info messages) (0||1)");
	puts  ("");
	puts  ("");
	puts  ("Config file:");
	puts  ("Mandatory options:");
	puts  ("  sd=string   Set sip provider domain.");
	puts  ("  su=string   Set sip username.");
	puts  ("  sp=string   Set sip password.");
	puts  ("  ln=string   Language identifier for espeak TTS (e.g. en = English or de = German)");
	puts  ("");
	puts  (" and at least one dtmf configuration (X = dtmf-key index):");
	puts  ("  dtmf.X.active=int           Set dtmf-setting active (0||1).");
	puts  ("  dtmf.X.description=string   Set description.");
	puts  ("  dtmf.X.tts-intro=string     Set tts intro.");
	puts  ("  dtmf.X.tts-answer=string    Set tts answer.");
	puts  ("  dtmf.X.cmd=string           Set dtmf command.");
	puts  ("");
	puts  ("Optional options:");
	puts  ("  rc=int      Record call (0||1)");
	puts  ("  af=string   announcement wav file to play; tts will not be read, if this parameter is given.");
	puts  ("              file format is Microsoft WAV (signed 16 bit) Mono, 22 kHz");
	puts  ("  cmd=string  command to check if the call should be taken");
	puts  ("              should return a \"1\" as first char, if yes.");
	puts  ("              the wildcard # will be replaced with the calling phone number in the command");
	puts  ("  am=string   aftermath: command to be executed after call ends. Will be called with two parameters: $1 = Phone number $2 = recorded file name");

	fflush(stdout);
}

// helper for parsing command-line-argument
static int try_get_argument(int arg, char *arg_id, char **arg_val, int argc, char *argv[])
{
	int found = 0;

	// check if actual argument is searched argument
	if (!strcasecmp(argv[arg], arg_id))
	{
		// check if actual argument has a value
		if (argc >= (arg+1))
		{
			// set value
			*arg_val = argv[arg+1];
			found = 1;
		}
	}
	return found;
}

// helper for parsing config file
static void parse_config_file(char *cfg_file)
{
	// open config file
	char line[200];
	FILE *file = fopen(cfg_file, "r");

	if (file!=NULL)
	{
		// start parsing file
		while(fgets(line, 200, file) != NULL)
		{
			char *arg, *val;

			// ignore comments and just new lines
			if(line[0] == '#') continue;
			if(line[0] == '\n') continue;

			// split string at '='-char
			arg = strtok(line, "=");
			if (arg == NULL) continue;
			val = strtok(NULL, "=");

			// check for new line char and remove it
			char *nl_check;
			nl_check = strstr (val, "\n");
			if (nl_check != NULL) strncpy(nl_check, " ", 1);

			// remove trailing spaces


			// duplicate string for having own instance of it
			char *arg_val = strdup(val);

			// check for sip domain argument
			if (!strcasecmp(arg, "sd"))
			{
				app_cfg.sip_domain = trim_string(arg_val);
				continue;
			}

			// check for sip user argument
			if (!strcasecmp(arg, "su"))
			{
				app_cfg.sip_user = trim_string(arg_val);
				continue;
			}

			// check for sip domain argument
			if (!strcasecmp(arg, "sp"))
			{
				app_cfg.sip_password = trim_string(arg_val);
				continue;
			}

			// check for language argument
			if (!strcasecmp(arg, "ln"))
			{
				app_cfg.language = trim_string(arg_val);
				continue;
			}

			// check for record calls argument
			if (!strcasecmp(arg, "rc"))
			{
				app_cfg.record_calls = atoi(val);
				continue;
			}

			// check for announcement file argument
			if (!strcasecmp(arg, "af"))
			{
				app_cfg.announcement_file = trim_string(arg_val);
				continue;
			}

			// check for call command
			if (!strcasecmp(arg, "cmd"))
			{
				app_cfg.CallCmd = trim_string(arg_val);
				continue;
			}

			// check for aftermath
			if (!strcasecmp(arg, "am"))
			{
				app_cfg.AfterMath = trim_string(arg_val);
				continue;
			}

			// check for silent mode argument
			if (!strcasecmp(arg, "s"))
			{
				app_cfg.silent_mode = atoi(val);
				continue;
			}

			// check for tts intro
			if (!strcasecmp(arg, "tts"))
			{
				app_cfg.tts = arg_val;
				continue;
			}

			// check for a dtmf argument
			char dtmf_id[1];
			char dtmf_setting[25];
			if(sscanf(arg, "dtmf.%1[^.].%s", dtmf_id, dtmf_setting) == 2)
			{
				// parse dtmf id (key)
				int d_id;
				d_id = atoi(dtmf_id);

				// check if actual dtmf id blasts maxium settings
				if (d_id >= MAX_DTMF_SETTINGS) continue;

				// get pointer to actual dtmf_cfg entry
				struct dtmf_config *d_cfg = &app_cfg.dtmf_cfg[d_id-1];

				// check for dtmf active setting
				if (!strcasecmp(dtmf_setting, "active"))
				{
					d_cfg->active = atoi(val);
					continue;
				}

				// check for dtmf description setting
				if (!strcasecmp(dtmf_setting, "description"))
				{
					d_cfg->description = arg_val;
					continue;
				}

				// check for dtmf tts intro setting
				if (!strcasecmp(dtmf_setting, "tts-intro"))
				{
					d_cfg->tts_intro = arg_val;
					continue;
				}

				// check for dtmf tts answer setting
				if (!strcasecmp(dtmf_setting, "tts-answer"))
				{
					d_cfg->tts_answer = arg_val;
					continue;
				}

				// check for dtmf cmd setting
				if (!strcasecmp(dtmf_setting, "cmd"))
				{
					d_cfg->cmd = arg_val;
					continue;
				}
			}

			// write warning if unknown configuration setting is found
			char warning[200];
			sprintf(warning, "Warning: Unknown configuration with arg '%s' and val '%s'\n", arg, val);
			log_message(warning);

		}

		fclose(file);
	}
	else
	{
		// return if config file not found
		log_message("Error while parsing config file: Not found.\n");
		exit(1);
	}
}

// helper for removing leading and trailing strings (source taken from kernel source, lib/string.c)
static char *trim_string(char *str)
{
	while (isspace(*str)) ++str;

	char *s = (char *)str;

	size_t size;
	char *end;

	size = strlen(s);
	if (!size) return s;

	end = s + size - 1;
	while (end >= s && isspace(*end)) end--;
	*(end + 1) = '\0';

	return s;
}


// helper for logging messages to console (disabled if silent mode is active)
static void log_message(char *message)
{
	if (!app_cfg.silent_mode)
	{
		fprintf(stderr, message);
	}
}

// helper for setting up sip library pjsua
static void setup_sip(void)
{
	pj_status_t status;

	log_message("Setting up pjsua ... ");

	// create pjsua
	status = pjsua_create();
	if (status != PJ_SUCCESS) error_exit("Error in pjsua_create()", status);

	// configure pjsua
	pjsua_config cfg;
	pjsua_config_default(&cfg);

	// enable just 1 simultaneous call
	cfg.max_calls = 1;

	// callback configuration
	cfg.cb.on_incoming_call = &on_incoming_call;
	cfg.cb.on_call_media_state = &on_call_media_state;
	cfg.cb.on_call_state = &on_call_state;
	cfg.cb.on_dtmf_digit = &on_dtmf_digit;

	// logging configuration
	pjsua_logging_config log_cfg;
	pjsua_logging_config_default(&log_cfg);
	log_cfg.console_level = PJSUA_LOG_LEVEL;

	// media configuration
	pjsua_media_config media_cfg;
	pjsua_media_config_default(&media_cfg);
	media_cfg.snd_play_latency = 100;
	media_cfg.clock_rate = 8000;
	media_cfg.snd_clock_rate = 8000;
	media_cfg.quality = 10;

	// initialize pjsua
	status = pjsua_init(&cfg, &log_cfg, &media_cfg);
	if (status != PJ_SUCCESS) error_exit("Error in pjsua_init()", status);

	// add udp transport
	pjsua_transport_config udpcfg;
	pjsua_transport_config_default(&udpcfg);

	udpcfg.port = 5060;
	status = pjsua_transport_create(PJSIP_TRANSPORT_UDP, &udpcfg, NULL);
	if (status != PJ_SUCCESS) error_exit("Error creating transport", status);

	// initialization is done, start pjsua
	status = pjsua_start();
	if (status != PJ_SUCCESS) error_exit("Error starting pjsua", status);

	// disable sound - use null sound device
	status = pjsua_set_null_snd_dev();
	if (status != PJ_SUCCESS) error_exit("Error disabling audio", status);

	log_message("Done.\n");
}

// helper for creating and registering sip-account
static void register_sip(void)
{
	pj_status_t status;

	log_message("Registering account ... ");

	// prepare account configuration
	pjsua_acc_config cfg;
	pjsua_acc_config_default(&cfg);

	// build sip-user-url
	char sip_user_url[40];
	sprintf(sip_user_url, "sip:%s@%s", app_cfg.sip_user, app_cfg.sip_domain);

	// build sip-provder-url
	char sip_provider_url[40];
	sprintf(sip_provider_url, "sip:%s", app_cfg.sip_domain);

	// create and define account
	cfg.id = pj_str(sip_user_url);
	cfg.reg_uri = pj_str(sip_provider_url);
	cfg.cred_count = 1;
	cfg.cred_info[0].realm = pj_str(app_cfg.sip_domain);
	cfg.cred_info[0].scheme = pj_str("digest");
	cfg.cred_info[0].username = pj_str(app_cfg.sip_user);
	cfg.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
	cfg.cred_info[0].data = pj_str(app_cfg.sip_password);

	// add account
	status = pjsua_acc_add(&cfg, PJ_TRUE, &acc_id);
	if (status != PJ_SUCCESS) error_exit("Error adding account", status);

	log_message("Done.\n");
}

// helper for creating call-media-player
static void create_player(pjsua_call_id call_id, char *file)
{
	// get call infos
	pjsua_call_info ci;
	pjsua_call_get_info(call_id, &ci);

	pj_str_t name;
	pj_status_t status = PJ_ENOTFOUND;

	log_message("Creating player ... ");

	// create player for playback media
	status = pjsua_player_create(pj_cstr(&name, file), PJMEDIA_FILE_NO_LOOP, &play_id);
	if (status != PJ_SUCCESS) error_exit("Error playing sound-playback", status);

	// connect active call to media player
	pjsua_conf_connect(pjsua_player_get_conf_port(play_id), ci.conf_slot);

	// get media port (play_port) from play_id
    status = pjsua_player_get_port(play_id, &play_port);
	if (status != PJ_SUCCESS) error_exit("Error getting sound player port", status);

	log_message("Done.\n");
}

// helper for creating call-recorder
static void create_recorder(pjsua_call_info ci)
{
	// specify target file
	pj_str_t rec_file = pj_str(rec_ans_file);
	pj_status_t status = PJ_ENOTFOUND;

	log_message("Creating recorder ... ");

	// Create recorder for call
	status = pjsua_recorder_create(&rec_file, 0, NULL, 0, 0, &rec_id); // don't forget to destroy recorder, to have the file written.
	if (status != PJ_SUCCESS) error_exit("Error recording answer", status);

	// connect active call to call recorder
	pjsua_conf_port_id rec_port = pjsua_recorder_get_conf_port(rec_id);
	pjsua_conf_connect(ci.conf_slot, rec_port);

	log_message("Done.\n");
}

void player_destroy(pjsua_player_id id) {
	if (id != PJSUA_INVALID_ID)
	{
		pjsua_player_destroy(id);
		play_id = PJSUA_INVALID_ID;
	}
}

int recorder_destroy(pjsua_player_id id) {
	if (id != PJSUA_INVALID_ID)
	{
		pjsua_recorder_destroy(id);
		rec_id = PJSUA_INVALID_ID;
		return 0;
	}
	return 1;
}

// synthesize speech / create message via espeak
static int synthesize_speech(char *speech, char *file, char* language)
{
	int speech_status = -1;

	char speech_command[1024];
	sprintf(speech_command, "espeak -v%s -a%i -k%i -s%i -p%i -w %s '%s'", language, ESPEAK_AMPLITUDE, ESPEAK_CAPITALS_PITCH, ESPEAK_SPEED, ESPEAK_PITCH, file, speech);
	speech_status = system(speech_command);

	return speech_status;
}


static void extractdelimited(char* dest, char* src, char cBeg, char cEnd)
{
	char* pBeg = strchr(src,cBeg);
	char* pEnd = strrchr(src,cEnd);
	if( pBeg == NULL || pEnd == NULL)
	{
		// leave dest alone.
		return;
	}
	int len = pEnd - pBeg;
	strncpy(dest,pBeg+1,len-1);
}

static void getTimestamp(char* dest)
{

	time_t ltime;
	struct tm *Tm;

	ltime=time(NULL);
	Tm=localtime(&ltime);

	sprintf(dest, "%04d-%02d-%02d %02d-%02d-%02d",
			Tm->tm_year+1900,
			Tm->tm_mon+1,
			Tm->tm_mday,
			Tm->tm_hour,
			Tm->tm_min,
			Tm->tm_sec);

}

static void stringRemoveChars(char *string, char *spanset) {
	char *ptr = string;
	ptr = strpbrk(ptr, spanset);

	while(ptr != NULL) {
		*ptr = '_';
		ptr = strpbrk(ptr, spanset);
	}
}

static void FileNameFromCallInfo(char* filename, char* sipNr, pjsua_call_info ci) {
	// log call info
	char sipTxt[100] = "";

	char PhoneBookText[100] = "NoEntry";
	char tmp[100];
	char* ptr;
	strcpy(tmp, ci.remote_info.ptr);

	// get elements
	extractdelimited(PhoneBookText, tmp, '\"', '\"');
	extractdelimited(sipTxt, tmp, '<', '>');

	// extract phone number
	if (strncmp(sipTxt, "sip:", 4) == 0) {
		int i = strcspn(sipTxt, "@") - 4;
		strncpy(sipNr, &sipTxt[4], i);
		sipNr[i] = '\0';
	} else {
		//sprintf(tmp,"SIP invalid");
		sprintf(tmp, "SIP does not start with sip:<%s>\n", sipTxt);
		log_message(tmp);
	}

	getTimestamp(tmp);

	// build filename
	strcpy(filename, tmp);
	strcat(filename, " ");
	strcat(filename, sipNr);
	if (strlen(PhoneBookText) > 0) {
		strcat(filename, " ");
		strcat(filename, PhoneBookText);
	}
	strcat(filename, ".wav");

	//sanitize string for filename
	stringRemoveChars(filename, "\":\\/*?|<>$%&'`{}[]()@");
}

#define RESULTSIZE 20

// helper for calling BASH
static int callBash(char* command, char* result) {

	int error=0;
	FILE* fp;

	fp = popen(command, "r");
	if (fp == NULL) {
		error = 1;
		log_message(" (Failed to run command) \n");
	}

	if (!error) {
		if (fgets(result, RESULTSIZE - 1, fp) == NULL) {
			error = 1;
			log_message(" (Failed to read result) \n");
		}
	}

	return error;
}

// handler for incoming-call-events
static void on_incoming_call(pjsua_acc_id acc_id, pjsua_call_id call_id, pjsip_rx_data *rdata)
{
	char info[200];
	char filename[200];
	char sipNr[100] = "";

	// get call infos
	pjsua_call_info ci;
	pjsua_call_get_info(call_id, &ci);

	PJ_UNUSED_ARG(acc_id);
	PJ_UNUSED_ARG(rdata);

	current_call = call_id;

	FileNameFromCallInfo(filename,sipNr,ci);

	// log call info
	sprintf(info, "Incoming call from |%s|\n>%s<\n",ci.remote_info.ptr,filename);
	log_message(info);

	// store filename for call into global variable for recorder
	strcpy(rec_ans_file, filename);
	strcpy(lastNumber, sipNr); // remember number as well

    // fire external job to check, if we take the call

    char result[RESULTSIZE];
    result[0] = '1'; result[1] = '\0'; // preset with "take call
    int error;

	if(app_cfg.CallCmd)
	{
		char cmdOut[200];
		char* cmd;
		int lLen;
		cmd = app_cfg.CallCmd; // copy ptr, just for beauty

		// modify cmd
		char* rHalf = strchr(cmd,'#'); // get ptr to right half of string (assuming one '#')
		if(rHalf != NULL)
		{
			// found'#'
			lLen = strcspn(cmd,"#");
			strncpy(cmdOut,cmd,lLen);
			cmdOut[lLen]='\0';
			strcat(cmdOut,sipNr);
			strcat(cmdOut,rHalf+1);
		}
		else
		{
			strcpy(cmdOut,cmd); // just copy
		}

		sprintf(info, "Checking with \"%s\"\n",cmdOut);
		log_message(info);

		error = callBash(cmdOut, result);

		sprintf(info, "check result:\n%s\n",result,error);
		log_message(info);
	}

	if(result[0]=='1')
	{
		// answer incoming call with 200 status/OK
		pjsua_call_answer(call_id, 200, NULL, NULL);
	}
	else
	{
		log_message("Will not take call.\n");
	}
}

// handler for call-media-state-change-events
static void on_call_media_state(pjsua_call_id call_id)
{
	// get call infos
	pjsua_call_info ci;
	pjsua_call_get_info(call_id, &ci);

	pj_status_t status = PJ_ENOTFOUND;

	// check state if call is established/active
	if (ci.media_status == PJSUA_CALL_MEDIA_ACTIVE) {

		log_message("Call media activated.\n");

		// create and start media player
		if(app_cfg.announcement_file)
		{
			create_player(call_id, app_cfg.announcement_file);
		}
		else
		{
			create_player(call_id, tts_file);
		}

		// create and start call recorder
		if (app_cfg.record_calls)
		{
			create_recorder(ci);
		}
	}
}

// handler for call-state-change-events
static void on_call_state(pjsua_call_id call_id, pjsip_event *e)
{
	// get call infos
	pjsua_call_info ci;
	pjsua_call_get_info(call_id, &ci);

	// prevent warning about unused argument e
    PJ_UNUSED_ARG(e);

	// check call state
	if (ci.state == PJSIP_INV_STATE_CONFIRMED)
	{
		log_message("Call confirmed.\n");

		// ensure that message is played from start
		if (play_id != PJSUA_INVALID_ID)
		{
			pjmedia_wav_player_port_set_pos(play_port, 0);
		}
	}
	if (ci.state == PJSIP_INV_STATE_DISCONNECTED)
	{
		log_message("Call disconnected.\n");

		// disable player
		player_destroy(play_id);
        // dont't forget the recorder!
		if(recorder_destroy(rec_id) == 0)
		{
			// ok, recorder has been destroyed successfully, there should be a file too.
			log_message("a file has been recorded.\n");

			// process the Aftermath, if we have any.
			if(app_cfg.AfterMath)
			{
				char result[RESULTSIZE];
				char command[300];
				sprintf(command,"%s \"%s\" \"%s\" \"%s\"", app_cfg.AfterMath, ci.local_info.ptr, lastNumber, rec_ans_file);

				log_message(command);
				log_message("\n");
				// do it.
				callBash(command, result);
			}
		}
	}
}





// handler for dtmf-events
static void on_dtmf_digit(pjsua_call_id call_id, int digit)
{
	// get call infos
	pjsua_call_info ci;
	pjsua_call_get_info(call_id, &ci);

	// work on detected dtmf digit
	int dtmf_key = digit - 48;

	char info[100];
	sprintf(info, "DTMF command detected: %i\n", dtmf_key);
	log_message(info);

	struct dtmf_config *d_cfg = &app_cfg.dtmf_cfg[dtmf_key-1];
	if (d_cfg->processing_active == 0)
	{
		d_cfg->processing_active = 1;

		if (d_cfg->active == 1)
		{
			log_message("Active DTMF command found for received digit.\n");
			log_message("Creating answer ... ");

			int error = 0;
			char command[100];
			char result[RESULTSIZE];

			strcpy(command, d_cfg->cmd);

			error = callBash(command, result);
			if (!error)
			{
				player_destroy(play_id);
				recorder_destroy(rec_id);

				char tts_buffer[200];
				sprintf(tts_buffer, d_cfg->tts_answer, result);

				int synth_status = -1;
				synth_status = synthesize_speech(tts_buffer, tts_answer_file, app_cfg.language);
				if (synth_status != 0) log_message(" (Failed to synthesize speech) ");

				create_player(call_id, tts_answer_file);
			}

			log_message("Done.\n");
		}
		else
		{
			log_message("No active DTMF command found for received digit.\n");
		}

		d_cfg->processing_active = 0;
	}
	else
	{
		log_message("DTMF command dropped - state is actual processing.\n");
	}
}

// handler for "break-in-key"-events (e.g. ctrl+c)
static void signal_handler(int signal)
{
	// exit app
	app_exit();
}

// clean application exit
static void app_exit()
{
	if (!app_exiting)
	{
		app_exiting = 1;
		log_message("Stopping application ... \n");

		// check if player/recorder is active and stop them
		player_destroy(play_id);
		recorder_destroy(rec_id);

		// hangup open calls and stop pjsua
		pjsua_call_hangup_all();
		pjsua_destroy();

		log_message("Done.\n");

		exit(0);
	}
}

// display error and exit application
static void error_exit(const char *title, pj_status_t status)
{
	if (!app_exiting)
	{
		app_exiting = 1;
		log_message("App Error Exit\n");

		pjsua_perror("SIP Call", title, status);

		// check if player/recorder is active and stop them
		player_destroy(play_id);
		recorder_destroy(rec_id);

		// hangup open calls and stop pjsua
		pjsua_call_hangup_all();
		pjsua_destroy();

		exit(1);
	}
}
