/****************************************************
 *
 * This file includes the implementation of the functions
 * made available to JavaScript. They are mainly convenience
 * functions around PocketSphinx API.
 *
 **************************************************/

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include "pocketsphinx.h"

/** Possible return values of most calls */
enum PsReturnType {SUCCESS = 0,
		   BAD_STATE = 1,
		   BAD_ARGUMENT = 2,
		   RUNTIME_ERROR = 3};

/** The recognizer uses static variables,
 * we keep track of the state
 */
enum PsState {UNINITIALIZED = 0,
	      INITIALIZED = 1,
	      INITIALIZING = 2,
	      ENTERING_GRAMMAR = 3,
	      RECORDING = 4};

static PsState psState = UNINITIALIZED;

// Implementation is based on static variables,
// there is no thread issue as there is no data
// shared between threads in JavaScript
static ps_decoder_t * recognizer = NULL;
static fsg_set_t * grammar_set = NULL;
static logmath_t * logmath = NULL;
static int32_t grammar_index = 0;
static fsg_model_t * current_grammar = NULL;
static std::vector<std::string> grammar_names;
static std::string current_hyp;
static int32 score;
static char const * sentence_id;

/**********************************
 *
 * This initializes a new recognizer
 *
 * @return 0 if successful, error code otherwise
 *
 *********************************/
int psInitializeImpl() {
  if (psState != UNINITIALIZED)
    return BAD_STATE;
  psState = INITIALIZING;
  // TODO: we need to keep grammar names in scope
  // while the recognizer is alive. Storing the stings
  // in a vector is fine until it gets expanded, making new
  // copies of the strings, invalidating the char* pointers
  grammar_names.reserve(1000);
  const arg_t cont_args_def[] = {
    POCKETSPHINX_OPTIONS,
    { "-argfile",
      ARG_STRING,
      NULL,
      "Argument file giving extra arguments." },
    { "-adcdev",
      ARG_STRING,
      NULL,
      "Name of audio device to use for input." },
    { "-infile",
      ARG_STRING,
      NULL,
      "Audio file to transcribe." },
    { "-time",
      ARG_BOOLEAN,
      "no",
      "Print word times in file transcription." },
    CMDLN_EMPTY_OPTION
  };
  // We will initialize the decoder with the provided acoustic model
  char * argv[] = {(char *)"-hmm", (char *)"am/rm1_200", (char *)"-bestpath", (char *)"no"};
  int argc = 4;
  // First, create a config with the provided arguments  
  cmd_ln_t * config = cmd_ln_parse_r(NULL, cont_args_def, argc, argv, FALSE);
  if (config == NULL) {
    psState = UNINITIALIZED;
    return RUNTIME_ERROR;
  }
  // Now it is time to initialize the decoder
  recognizer = ps_init(config);
  if (recognizer == NULL) {
    psState = UNINITIALIZED;
    return RUNTIME_ERROR;
  }
  logmath = logmath_init(1.0001, 0, 0);
  if (logmath == NULL) {
    psState = UNINITIALIZED;
    return RUNTIME_ERROR;
  }
  grammar_set = ps_update_fsgset(recognizer);
  if (grammar_set == NULL) {
    psState = UNINITIALIZED;
    return RUNTIME_ERROR;
  }
  psState = INITIALIZED;
  current_hyp = "";
  return SUCCESS;
}

/**************************************
 *
 * Retrieves the current state
 * @return current state
 *
 *************************************/
int psGetStateImpl() {return psState;}

/**************************************
 *
 * Retrieves the current hypothesis
 * @return current hypothesis
 *
 *************************************/
const char* psGetHypImpl() {
  return current_hyp.c_str();
}

/***********************************
 *
 * Starts inputing a new grammar
 *
 * @param numStates, the number of states of the grammar
 * @return 0 if successful, error code otherwise
 * 
 ***********************************/
int psStartGrammarImpl(int numStates) {
  if (psState != INITIALIZED)
    return BAD_STATE;
  std::ostringstream grammar_name;
  grammar_name << grammar_index;
  grammar_names.push_back(grammar_name.str());
  current_grammar = fsg_model_init(grammar_names.back().c_str(), logmath, 1.0, numStates);
  if (current_grammar == NULL)
    return RUNTIME_ERROR;
  current_grammar->start_state = 0;
  current_grammar->final_state = numStates - 1;
  psState = ENTERING_GRAMMAR;
  return SUCCESS;
}


/*******************************************
 *
 * Finishes inputing a grammar
 * @param pointer to host the value of the id of the grammar
 * @return 0 if successful, error code otherwise
 *
 *****************************************/
int psEndGrammarImpl(int32_t *id) {
  if (psState != ENTERING_GRAMMAR)
    return BAD_STATE;
  fsg_model_add_silence(current_grammar, "<sil>", -1, 1.0);
  if (current_grammar != fsg_set_add(grammar_set, grammar_names.back().c_str(), current_grammar)) {
    return RUNTIME_ERROR;
  }
  *id = grammar_index;
  grammar_index++;
  ps_update_fsgset(recognizer);
  fsg_model_t * fsg = fsg_set_select(grammar_set, grammar_names.back().c_str());
  if (fsg == NULL)
    return RUNTIME_ERROR;
  if (ps_update_fsgset(recognizer) == NULL)
    return RUNTIME_ERROR;
  psState = INITIALIZED;
  return SUCCESS;
}

/*******************************************
 *
 * Switches the recognizer to the given grammar
 * @param id of the grammar to switch to
 * @return 0 if successful, error code otherwise
 *
 *****************************************/
int psSwitchGrammarImpl(int id) {
  if (psState != INITIALIZED)
    return BAD_STATE;
  std::ostringstream grammar_name;
  grammar_name << id;
  fsg_model_t * fsg = fsg_set_select(grammar_set, grammar_name.str().c_str());
  if (fsg == NULL)
    return RUNTIME_ERROR;
  if (ps_update_fsgset(recognizer) == NULL)
    return RUNTIME_ERROR;
  return SUCCESS;
}

/*******************************************
 *
 * Adds a new word to the dictionary
 * @param word to add
 * @param pronunciation of the word to add
 * @return 0 if successful, error code otherwise
 *
 *****************************************/
int psAddWordImpl(char *word, char *pronunciation) {
  if (psState != INITIALIZED)
    return BAD_STATE;
  if (recognizer == NULL) {
    return RUNTIME_ERROR;
  }
  if (ps_add_word(recognizer, word, pronunciation, 1) < 0)
    return RUNTIME_ERROR;
  return SUCCESS;
}

/*******************************************
 *
 * Adds a transition to the grammar currently being entered
 * @param starting state of the transition
 * @param destination state of the transition
 * @param word associated to the transition
 * @return 0 if successful, error code otherwise
 *
 *****************************************/
int psAddTransitionImpl(int fromState, int toState, char* word) {
  if (psState != ENTERING_GRAMMAR)
    return BAD_STATE;
  if ((fromState > current_grammar->final_state) || (toState > current_grammar->final_state))
    return BAD_ARGUMENT;
  fsg_model_trans_add(current_grammar, fromState, toState, 0, fsg_model_word_add(current_grammar, word));
  return SUCCESS;
}

/*******************************************
 *
 * Starts recognition on the current grammar
 * @return 0 if successful, error code otherwise
 *
 *****************************************/
int psStartImpl() {
  if (psState != INITIALIZED)
    return BAD_STATE;
  if (ps_start_utt(recognizer, NULL) < 0) {
    return RUNTIME_ERROR;
  }
  current_hyp = "";
  psState = RECORDING;
  return SUCCESS;
}

/*******************************************
 *
 * Stops recognition
 * @return 0 if successful, error code otherwise
 *
 *****************************************/
int psStopImpl() {
  if (psState != RECORDING)
    return BAD_STATE;
  if (ps_end_utt(recognizer) < 0) {
    return RUNTIME_ERROR;
  }
  const char* h = ps_get_hyp(recognizer, &score, &sentence_id);
  current_hyp = (h == NULL) ? "" : h;
  psState = INITIALIZED;
  return SUCCESS;
}

/*******************************************
 *
 * Processes the given audio samples
 * @param pointer ot an array of audio samples (short integers)
 * @param size of the array
 * @return 0 if successful, error code otherwise
 *
 *****************************************/
int psProcessImpl(void* data, int length) {
  if (psState != RECORDING)
    return BAD_STATE;
  if ((data == NULL) || (length == 0))
    return RUNTIME_ERROR;
  ps_process_raw(recognizer, (short int *) data, length, 0, 0);
  const char* h = ps_get_hyp(recognizer, &score, &sentence_id);
  current_hyp = (h == NULL) ? "" : h;
  return SUCCESS;
}
