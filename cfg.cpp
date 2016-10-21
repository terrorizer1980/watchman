/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static json_ref global_cfg;
static json_ref arg_cfg;
static pthread_rwlock_t cfg_lock = PTHREAD_RWLOCK_INITIALIZER;

/* Called during shutdown to free things so that we run cleanly
 * under valgrind */
void cfg_shutdown(void)
{
  global_cfg.reset();
  arg_cfg.reset();
}

void cfg_load_global_config_file(void)
{
  json_error_t err;

  const char *cfg_file = getenv("WATCHMAN_CONFIG_FILE");
#ifdef WATCHMAN_CONFIG_FILE
  if (!cfg_file) {
    cfg_file = WATCHMAN_CONFIG_FILE;
  }
#endif
  if (!cfg_file || cfg_file[0] == '\0') {
    return;
  }

  if (!w_path_exists(cfg_file)) {
    return;
  }

  auto config = json_load_file(cfg_file, 0, &err);
  if (!config) {
    w_log(W_LOG_ERR, "failed to parse json from %s: %s\n",
          cfg_file, err.text);
    return;
  }

  global_cfg = config;
}

void cfg_set_arg(const char *name, json_t *val)
{
  pthread_rwlock_wrlock(&cfg_lock);
  if (!arg_cfg) {
    arg_cfg = json_object();
  }

  json_object_set_nocheck(arg_cfg, name, val);

  pthread_rwlock_unlock(&cfg_lock);
}

void cfg_set_global(const char *name, json_t *val)
{
  pthread_rwlock_wrlock(&cfg_lock);
  if (!global_cfg) {
    global_cfg = json_object();
  }

  json_object_set_nocheck(global_cfg, name, val);

  pthread_rwlock_unlock(&cfg_lock);
}

static json_t* cfg_get_raw(const char* name, json_ref* optr) {
  json_t *val = NULL;

  pthread_rwlock_rdlock(&cfg_lock);
  if (*optr) {
    val = json_object_get(*optr, name);
  }
  pthread_rwlock_unlock(&cfg_lock);
  return val;
}

json_t *cfg_get_json(const w_root_t *root, const char *name)
{
  json_t *val = NULL;

  // Highest precedence: options set on the root
  if (root && root->config_file) {
    val = json_object_get(root->config_file, name);
  }
  // then: command line arguments
  if (!val) {
    val = cfg_get_raw(name, &arg_cfg);
  }
  // then: global config options
  if (!val) {
    val = cfg_get_raw(name, &global_cfg);
  }
  return val;
}

const char *cfg_get_string(const w_root_t *root, const char *name,
    const char *defval)
{
  json_t *val = cfg_get_json(root, name);

  if (val) {
    if (!json_is_string(val)) {
      w_log(W_LOG_FATAL, "Expected config value %s to be a string\n", name);
    }
    return json_string_value(val);
  }

  return defval;
}

// Return true if the json ref is an array of string values
static bool is_array_of_strings(json_t *ref) {
  uint32_t i;

  if (!json_is_array(ref)) {
    return false;
  }

  for (i = 0; i < json_array_size(ref); i++) {
    if (!json_is_string(json_array_get(ref, i))) {
      return false;
    }
  }
  return true;
}

// Given an array of string values, if that array does not contain
// a ".watchmanconfig" entry, prepend it
static void prepend_watchmanconfig_to_array(json_t *ref) {
  const char *val;

  if (json_array_size(ref) == 0) {
    // json_array_insert_new at index can fail when the array is empty,
    // so just append in this case.
    json_array_append_new(ref, typed_string_to_json(".watchmanconfig",
          W_STRING_UNICODE));
    return;
  }

  val = json_string_value(json_array_get(ref, 0));
  if (!strcmp(val, ".watchmanconfig")) {
    return;
  }
  json_array_insert_new(ref, 0, typed_string_to_json(".watchmanconfig",
        W_STRING_UNICODE));
}

// Compute the effective value of the root_files configuration and
// return a json reference.  The caller must decref the ref when done
// (we may synthesize this value).   Sets enforcing to indicate whether
// we will only allow watches on the root_files.
// The array returned by this function (if not NULL) is guaranteed to
// list .watchmanconfig as its zeroth element.
json_ref cfg_compute_root_files(bool* enforcing) {
  *enforcing = false;

  json_ref ref = cfg_get_json(nullptr, "enforce_root_files");
  if (ref) {
    if (!json_is_boolean(ref)) {
      w_log(W_LOG_FATAL,
          "Expected config value enforce_root_files to be boolean\n");
    }
    *enforcing = json_is_true(ref);
  }

  ref = cfg_get_json(NULL, "root_files");
  if (ref) {
    if (!is_array_of_strings(ref)) {
      w_log(W_LOG_FATAL,
          "global config root_files must be an array of strings\n");
      *enforcing = false;
      return nullptr;
    }
    prepend_watchmanconfig_to_array(ref);

    return ref;
  }

  // Try legacy root_restrict_files configuration
  ref = cfg_get_json(NULL, "root_restrict_files");
  if (ref) {
    if (!is_array_of_strings(ref)) {
      w_log(W_LOG_FATAL, "deprecated global config root_restrict_files "
          "must be an array of strings\n");
      *enforcing = false;
      return nullptr;
    }
    prepend_watchmanconfig_to_array(ref);
    *enforcing = true;
    return ref;
  }

  // Synthesize our conservative default value.
  // .watchmanconfig MUST be first
  return json_pack("[ssss]", ".watchmanconfig", ".hg", ".git", ".svn");
}

json_int_t cfg_get_int(const w_root_t *root, const char *name,
    json_int_t defval)
{
  json_t *val = cfg_get_json(root, name);

  if (val) {
    if (!json_is_integer(val)) {
      w_log(W_LOG_FATAL, "Expected config value %s to be an integer\n", name);
    }
    return json_integer_value(val);
  }

  return defval;
}

bool cfg_get_bool(const w_root_t *root, const char *name, bool defval)
{
  json_t *val = cfg_get_json(root, name);

  if (val) {
    if (!json_is_boolean(val)) {
      w_log(W_LOG_FATAL, "Expected config value %s to be a boolean\n", name);
    }
    return json_is_true(val);
  }

  return defval;
}

double cfg_get_double(const w_root_t *root, const char *name, double defval) {
  json_t *val = cfg_get_json(root, name);

  if (val) {
    if (!json_is_number(val)) {
      w_log(W_LOG_FATAL, "Expected config value %s to be a number\n", name);
    }
    return json_real_value(val);
  }

  return defval;
}

#define MAKE_GET_PERM(PROP, SUFFIX) \
  static mode_t get_ ## PROP ## _perm(const char *name, json_t *val, \
                                      bool write_bits, bool execute_bits) { \
    mode_t ret = 0; \
    json_t *perm = json_object_get(val, #PROP); \
    if (perm) { \
      if (!json_is_boolean(perm)) { \
        w_log(W_LOG_FATAL, "Expected config value %s." #PROP \
              " to be a boolean\n", name); \
      } \
      if (json_is_true(perm)) { \
        ret |= S_IR ## SUFFIX; \
        if (write_bits) { \
          ret |= S_IW ## SUFFIX; \
        } \
        if (execute_bits) { \
          ret |= S_IX ## SUFFIX; \
        } \
      } \
    } \
    return ret; \
  }

MAKE_GET_PERM(group, GRP)
MAKE_GET_PERM(others, OTH)

/**
 * This function expects the config to be an object containing the keys 'group'
 * and 'others', each a bool.
 */
mode_t cfg_get_perms(const w_root_t *root, const char *name, bool write_bits,
                     bool execute_bits) {
  json_t *val = cfg_get_json(root, name);
  mode_t ret = S_IRUSR | S_IWUSR;
  if (execute_bits) {
    ret |= S_IXUSR;
  }

  if (val) {
    if (!json_is_object(val)) {
      w_log(W_LOG_FATAL, "Expected config value %s to be an object\n", name);
    }

    ret |= get_group_perm(name, val, write_bits, execute_bits);
    ret |= get_others_perm(name, val, write_bits, execute_bits);
  }

  return ret;
}

const char *cfg_get_trouble_url(void) {
  return cfg_get_string(NULL, "troubleshooting_url",
    "https://facebook.github.io/watchman/docs/troubleshooting.html");
}

/* vim:ts=2:sw=2:et:
 */
