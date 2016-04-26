/*
 *
 * Copyright (C)2000-2015 Sensory Inc.
 * Copyright (C) 2016 Canonical
 *
 *---------------------------------------------------------------------------
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <trulyhandsfree.h>

#include <console.h>
#include <audio.h>
#include <datalocations.h>

#define RECOGNITION_OUT_FILE "/speech.recognition"

#define GENDER_MODEL "hbg_genderModel.raw"
#define SPHERO_NETFILE      "sy_home_auto2_home_auto2_en_US_sfs14_delivery01_am.raw"
#define SPHERO_SEARCHFILE   "sy_home_auto2_home_auto2_en_US_sfs14_delivery01_search_12.raw" // pre-built search

#define NBEST              (1)                /* Number of results */
#define PHRASESPOT_PARAMA_OFFSET   (0)        /* Phrasespotting ParamA Offset */
#define PHRASESPOT_BEAM    (100.f)            /* Pruning beam */
#define PHRASESPOT_ABSBEAM (100.f)            /* Pruning absolute beam */
#define PHRASESPOT_DELAY  90                 /* Phrasespotting Delay */
#define MAXSTR             (512)              /* Output string size */

#define SEQ_BUFFER_MS     1000

#define THROW(a) { ewhere=(a); goto error; }
#define THROW2(a,b) {ewhere=(a); ewhat=(b); goto error; }


char *formatExpirationDate(time_t expiration)
{
  static char expdate[33];
  if (!expiration) return "never";
  strftime(expdate, 32, "%m/%d/%Y 00:00:00 GMT", gmtime(&expiration));
  return expdate;
}

int kbhit()
{
  struct timeval tv = { 0L, 0L };
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(0, &fds);
  return select(1, &fds, NULL, NULL, &tv);
}

char* concat(const char *s1, const char *s2)
{
    char *result = malloc(strlen(s1)+strlen(s2)+1);//+1 for the zero-terminator
    strcpy(result, s1);
    strcat(result, s2);
    return result;
}

void saveCommand(const char *dataPath, const char *text)
{
  char* recognitionFilePath = concat(dataPath, RECOGNITION_OUT_FILE);
  char* tempRecognitionFilePath = concat(recognitionFilePath, ".new");
  FILE *f = fopen(tempRecognitionFilePath, "w");
  if (f == NULL)
  {
      printf("Error opening result file!\n");
  }

  fprintf(f, "%s", text);
  fclose(f);

  int ret = rename(tempRecognitionFilePath, recognitionFilePath);
  if (ret != 0) {
    printf("Error: unable to save result file\n");
  }
  free(recognitionFilePath);
  free(tempRecognitionFilePath);
}

int main(int argc, char **argv)
{
  char speechRecognitionInput[255];
  char *dataPath = NULL;
  const char *rres;
  thf_t *ses=NULL;
  recog_t *r=NULL;
  searchs_t *s=NULL;
  pronuns_t *pp=NULL;
  char str[MAXSTR];
  const char *ewhere, *ewhat=NULL;
  float score;
  unsigned short status, done=0;
  audiol_t *p;
  unsigned short i=0;
  void *cons=NULL;
  FILE *fp=NULL;

  /* Draw console */
  if (!(cons=initConsole(NULL))) return 1;

  dataPath = getenv("SNAP_APP_DATA_PATH");
  if (!dataPath) {
    dataPath = getenv("PWD");
  }

  /* enable choosing a model selection file */
  strcpy(speechRecognitionInput, SPHERO_SEARCHFILE);
  char* speechModelChoiceFilePath = concat(dataPath, "/speechrecognition.input");

  FILE *speechModelChoiceFile = fopen(speechModelChoiceFilePath, "r");
  if (speechModelChoiceFile != NULL) {
    disp(cons, "Default model choice overriden\n");
    fscanf(speechModelChoiceFile, "%s", speechRecognitionInput);
    fclose(speechModelChoiceFile);
  } else {
    dispv(cons, "You can point to any .raw model file by creating %s containing the desired file name.\n", speechModelChoiceFilePath);
  }
  free(speechModelChoiceFilePath);

  /* Create SDK session */
  if(!(ses=thfSessionCreate())) {
    panic(cons, "thfSessionCreate", thfGetLastError(NULL), 0);
    return 1;
  }

  /* Display TrulyHandsfree SDK library version number */
  dispv(cons, "TrulyHandsfree SDK Library version: %s\n", thfVersion());
  dispv(cons, "Expiration date: %s\n\n",
	formatExpirationDate(thfGetLicenseExpiration()));

  disp(cons,"Loading recognizer: "  SPHERO_NETFILE);
  if(!(r=thfRecogCreateFromFile(ses, SPHERO_NETFILE, (unsigned short)(AUDIO_BUFFERSZ/1000.f*SAMPLERATE), -1, NO_SDET)))
    THROW("thfRecogCreateFromFile");

  dispv(cons,"Using custom search: %s\n", speechRecognitionInput);
  if(!(s=thfSearchCreateFromFile(ses,r,speechRecognitionInput,NBEST)))
    THROW("thfSearchCreateFromFile");


  /* Initialize recognizer */
  disp(cons,"Initializing recognizer...");

  if(!thfRecogInit(ses,r,s,RECOG_KEEP_WAVE_WORD_PHONEME))
    THROW("thfRecogInit");

  /* Configure parameters - only DELAY... others are saved in search already */
  disp(cons,"Setting parameters...");
  if (!thfPhrasespotConfigSet(ses,r,s,PS_DELAY,PHRASESPOT_DELAY))
    THROW("thfPhrasespotConfigSet: delay");

  // One second sequential buffer
  if (!thfPhrasespotConfigSet(ses, r, s, PS_SEQ_BUFFER, SEQ_BUFFER_MS))
    THROW("thfPhrasespotConfigSet:trigger:PS_SEQ_BUFFER");

  if (thfRecogGetSampleRate(ses,r) != SAMPLERATE)
    THROW("Acoustic model is incompatible with audio samplerate");



  /* initialize a speaker object with a single speaker,
   * and arbitrarily set it up
   * to hold one recording from this speaker
   */
  if (!thfSpeakerInit(ses,r,0,1)) {
    THROW("thfSpeakerInit");
  }

  disp(cons,"Loading gender model: " GENDER_MODEL);
  /* now read the gender model, which has been tuned to the target phrase */
  if (!thfSpeakerReadGenderModel(ses,r,GENDER_MODEL)) {
    THROW("thfSpeakerReadGenderModel");
  }

  /* Initialize audio */
  disp(cons,"Initializing audio...");
  initAudio(NULL,cons);

  disp(cons,"\nSupported commands:");
  disp(cons,"turn_on_the_light|Turn On The Light|Light On\n"
            "turn_the_light_off|Turn The Light Off|Light Off\n"
            "turn_on_the_air_conditioning|Turn On The Air Conditioning|AC On\n"
            "turn_the_air_conditioning_off|Turn The Air Conditioning Off|AC Off\n"
            "open_garage_door|Open Garage Door|Opening Garage Door\n"
            "close_garage|Close Garage|Closing Garage\n"
            //"whats_the_temperature|What's The Temperature|74 Degrees\n"
            "kitchen_turn_on_light|Kitchen Turn On Light|Kitchen Light On\n"
            "kitchen_turn_the_light_off|Kitchen Turn The Light Off|Kitchen Light Off");

  while (1) { // loop forever until jump out to exit
    disp(cons,"\n===== Ready for new command =====");
    if (startRecord() == RECOG_NODATA)
      break;

    /* Pipelined recognition */
    done=0;
    while (!done) {
      //abort if enter hit
      /*if (kbhit() == 1) {
        stopRecord();
        goto exit;
      }*/
      audioEventLoop();
      while ((p = audioGetNextBuffer(&done)) && !done) {
        if (!thfRecogPipe(ses, r, p->len, p->audio, RECOG_ONLY, &status))
          THROW("recogPipe");
        audioNukeBuffer(p);
        if (status == RECOG_DONE) {
          done = 1;
          stopRecord();
        }
      }
    }

    /* Report N-best recognition result */
    rres=NULL;
    if (status==RECOG_DONE) {
      disp(cons,"Recognition results...");
      score=0;
      if (!thfRecogResult(ses,r,&score,&rres,NULL,NULL,NULL,NULL,NULL,NULL))
        THROW("thfRecogResult");
      sprintf(str,"Result: %s (%.3f)",rres,score); disp(cons,str);

      {
        float genderProb;
        if (!thfSpeakerGender(ses, r, &genderProb)) {
          THROW("thfSpeakerGender");
        }
        if (genderProb < 0.5) {
          sprintf(str, "Gender: MALE (score=%f)\n", (1.0 - genderProb));
        }
        else {
          sprintf(str, "Gender: FEMALE (score=%f)\n", genderProb);
        }
        disp(cons, str);
      }

    } else
      disp(cons,"No recognition result");

    if (rres) {
      saveCommand(dataPath, rres);
    }
    thfRecogReset(ses,r);
  }
  /* Clean up */
exit:
  killAudio();
  thfRecogDestroy(r); r=NULL;
  thfSearchDestroy(s); s=NULL;
  thfPronunDestroy(pp); pp=NULL;
  thfSessionDestroy(ses); ses=NULL;
  disp(cons,"Done");
  closeConsole(cons);

  return 0;

  /* Async error handling, see console.h for panic */
 error:
  killAudio();
  if(!ewhat && ses) ewhat=thfGetLastError(ses);
  if(fp) fclose(fp);
  if(r) thfRecogDestroy(r);
  if(s) thfSearchDestroy(s);
  if(pp) thfPronunDestroy(pp);
  panic(cons,ewhere,ewhat,0);
  if(ses) thfSessionDestroy(ses);
  return 1;
}
