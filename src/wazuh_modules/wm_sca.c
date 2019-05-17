/*
 * Wazuh Module for Security Configuration Assessment
 * Copyright (C) 2015-2019, Wazuh Inc.
 * January 25, 2019.
 *
 * This program is a free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */

#include "wmodules.h"
#include <os_net/os_net.h>
#include <sys/stat.h>
#include "os_crypto/sha256/sha256_op.h"
#include "shared.h"


#undef minfo
#undef mwarn
#undef merror
#undef mdebug1
#undef mdebug2

#define minfo(msg, ...) _mtinfo(WM_SCA_LOGTAG, __FILE__, __LINE__, __func__, msg, ##__VA_ARGS__)
#define mwarn(msg, ...) _mtwarn(WM_SCA_LOGTAG, __FILE__, __LINE__, __func__, msg, ##__VA_ARGS__)
#define merror(msg, ...) _mterror(WM_SCA_LOGTAG, __FILE__, __LINE__, __func__, msg, ##__VA_ARGS__)
#define mdebug1(msg, ...) _mtdebug1(WM_SCA_LOGTAG, __FILE__, __LINE__, __func__, msg, ##__VA_ARGS__)
#define mdebug2(msg, ...) _mtdebug2(WM_SCA_LOGTAG, __FILE__, __LINE__, __func__, msg, ##__VA_ARGS__)

typedef struct cis_db_info_t {
    char *result;
    cJSON *event;
} cis_db_info_t;

typedef struct cis_db_hash_info_t {
    cis_db_info_t **elem;
} cis_db_hash_info_t;

typedef struct request_dump_t {
    int policy_index;
    int first_scan;
} request_dump_t;

static int RETURN_NOT_FOUND = 0;
static int RETURN_FOUND = 1;
static int RETURN_INVALID = 2;

static void * wm_sca_main(wm_sca_t * data);   // Module main function. It won't return
static void wm_sca_destroy(wm_sca_t * data);  // Destroy data
static int wm_sca_start(wm_sca_t * data);  // Start
static cJSON *wm_sca_build_event(const cJSON * const profile, const cJSON * const policy, char **p_alert_msg, int id, const char * const result, const char * const reason);
static int wm_sca_send_event_check(wm_sca_t * data,cJSON *event);  // Send check event
static void wm_sca_read_files(wm_sca_t * data);  // Read policy monitoring files
static int wm_sca_do_scan(cJSON *profile_check,OSStore *vars,wm_sca_t * data,int id,cJSON *policy,int requirements_scan,int cis_db_index,unsigned int remote_policy,int first_scan, int *checks_number);
static int wm_sca_send_summary(wm_sca_t * data, int scan_id,unsigned int passed, unsigned int failed,unsigned int invalid,cJSON *policy,int start_time,int end_time, char * integrity_hash, char * integrity_hash_file, int first_scan, int id, int checks_number);
static int wm_sca_check_policy(const cJSON * const policy, const cJSON * const profiles);
static int wm_sca_check_requirements(const cJSON * const requirements);
static void wm_sca_summary_increment_passed();
static void wm_sca_summary_increment_failed();
static void wm_sca_summary_increment_invalid();
static void wm_sca_reset_summary();
static int wm_sca_send_alert(wm_sca_t * data,cJSON *json_alert); // Send alert
static int wm_sca_check_hash(OSHash *cis_db_hash, const char * const result, const cJSON * const profile, const cJSON * const event, int check_index, int policy_index);
static char *wm_sca_hash_integrity(int policy_index);
static char *wm_sca_hash_integrity_file(const char *file);
static void wm_sca_free_hash_data(cis_db_info_t *event);
static void * wm_sca_dump_db_thread(wm_sca_t * data);
static void wm_sca_send_policies_scanned(wm_sca_t * data);
static int wm_sca_send_dump_end(wm_sca_t * data, unsigned int elements_sent,char * policy_id,int scan_id);  // Send dump end event
static int append_msg_to_vm_scat (wm_sca_t * const data, const char * const msg);

#ifndef WIN32
static void * wm_sca_request_thread(wm_sca_t * data);
#endif

/* Extra functions */
static int wm_sca_get_vars(const cJSON * const variables, OSStore * const vars);
static void wm_sca_set_condition(const char * const c_cond, int *condition); // Set condition
static char * wm_sca_get_value(char *buf, int *type); // Get value
static char * wm_sca_get_pattern(char *value); // Get pattern
static int wm_sca_check_file_contents(const char * const file, const char * const pattern, char **reason);
static int wm_sca_check_file_list_for_contents(const char * const file_list, char * const pattern, char **reason);
static int wm_sca_check_file_existence(const char * const file, char **reason);
static int wm_sca_check_file_list_for_existence(const char * const file_list, char **reason);
static int wm_sca_check_file_list(const char * const file_list, char * const pattern, char **reason);
static int wm_sca_read_command(char *command, char *pattern, wm_sca_t * data, char **reason); // Read command output
static int wm_sca_pt_matches(const char * const str, const char * const pattern); // Check pattern match
static int wm_sca_check_dir(const char * const dir, const char * const file, char * const pattern, char **reason); // Check dir
static int wm_sca_check_dir_existence(const char * const dir, char **reason);
static int wm_sca_check_dir_list(wm_sca_t * const data, char * const dir_list, char * const file, char * const pattern, char **reason);
static int wm_sca_check_process_is_running(OSList *p_list, char *value); // Check process

#ifdef WIN32
static int wm_check_registry_entry(char * const value, char **reason);
static int wm_sca_is_registry(char *entry_name, char *reg_option, char *reg_value, char **reason);
static char *wm_sca_os_winreg_getkey(char *reg_entry);
static int wm_sca_test_key(char *subkey, char *full_key_name, unsigned long arch,char *reg_option, char *reg_value, char **reason);
static int wm_sca_winreg_querykey(HKEY hKey, const char *full_key_name, char *reg_option, char *reg_value, char **reason);
static char *wm_sca_getrootdir(char *root_dir, int dir_size);
#endif

cJSON *wm_sca_dump(const wm_sca_t * data);     // Read config

const wm_context WM_SCA_CONTEXT = {
    SCA_WM_NAME,
    (wm_routine)wm_sca_main,
    (wm_routine)(void *)wm_sca_destroy,
    (cJSON * (*)(const void *))wm_sca_dump
};

static unsigned int summary_passed = 0;
static unsigned int summary_failed = 0;
static unsigned int summary_invalid = 0;

OSHash **cis_db;
char **last_sha256;
cis_db_hash_info_t *cis_db_for_hash;

static w_queue_t * request_queue;
static wm_sca_t * data_win;

cJSON **last_summary_json = NULL;

/* Multiple readers / one write mutex */
static pthread_rwlock_t dump_rwlock;

// Module main function. It won't return
void * wm_sca_main(wm_sca_t * data) {
    // If module is disabled, exit
    if (data->enabled) {
        minfo("Module started.");
    } else {
        minfo("Module disabled. Exiting.");
        pthread_exit(NULL);
    }

    if (!data->profile || data->profile[0] == NULL) {
        minfo("No policies defined. Exiting.");
        pthread_exit(NULL);
    }

    data->msg_delay = 1000000 / wm_max_eps;
    data->summary_delay = 3; /* Seconds to wait for summary sending */
    data_win = data;

    /* Reading the internal options */

    // Default values
    data->request_db_interval = 300;
    data->remote_commands = 0;
    data->commands_timeout = 30;

    data->request_db_interval = getDefine_Int("sca","request_db_interval", 1, 60) * 60;
    data->commands_timeout = getDefine_Int("sca", "commands_timeout", 1, 300);
#ifdef CLIENT
    data->remote_commands = getDefine_Int("sca", "remote_commands", 0, 1);
#else
    data->remote_commands = 1;  // Only for agents
#endif

    /* Maximum request interval is the scan interval */
    if(data->request_db_interval > data->interval) {
       data->request_db_interval = data->interval;
       minfo("The request_db_interval option cannot be higher than the scan interval. It will be redefined to that value.");
    }

    /* Create Hash for each policy file */
    int i;
    if(data->profile){
        for(i = 0; data->profile[i]; i++) {
            os_realloc(cis_db, (i + 2) * sizeof(OSHash *), cis_db);
            cis_db[i] = OSHash_Create();
            if (!cis_db[i]) {
                merror(LIST_ERROR);
                pthread_exit(NULL);
            }
            OSHash_SetFreeDataPointer(cis_db[i], (void (*)(void *))wm_sca_free_hash_data);

            /* DB for calculating hash only */
            os_realloc(cis_db_for_hash, (i + 2) * sizeof(cis_db_hash_info_t), cis_db_for_hash);

            /* Last summary for each policy */
            os_realloc(last_summary_json, (i + 2) * sizeof(cJSON *), last_summary_json);
            last_summary_json[i] = NULL;

            /* Prepare first ID for each policy file */
            os_calloc(1,sizeof(cis_db_info_t *),cis_db_for_hash[i].elem);
            cis_db_for_hash[i].elem[0] = NULL;
        }
    }

    /* Create summary hash for each policy file */
    if(data->profile){
        for(i = 0; data->profile[i]; i++) {
            os_realloc(last_sha256, (i + 2) * sizeof(char *), last_sha256);
            os_calloc(1,sizeof(os_sha256),last_sha256[i]);
        }
    }

#ifndef WIN32

    for (i = 0; (data->queue = StartMQ(DEFAULTQPATH, WRITE)) < 0 && i < WM_MAX_ATTEMPTS; i++)
        wm_delay(1000 * WM_MAX_WAIT);

    if (i == WM_MAX_ATTEMPTS) {
        merror("Can't connect to queue.");
    }

#endif

    request_queue = queue_init(1024);

    w_rwlock_init(&dump_rwlock, NULL);

#ifndef WIN32
    w_create_thread(wm_sca_request_thread, data);
    w_create_thread(wm_sca_dump_db_thread, data);
#else
    if (CreateThread(NULL,
                    0,
                    (LPTHREAD_START_ROUTINE)wm_sca_dump_db_thread,
                    data,
                    0,
                    NULL) == NULL) {
                    merror(THREAD_ERROR);
    }
#endif

    wm_sca_start(data);

    return NULL;
}

static int wm_sca_send_alert(wm_sca_t * data,cJSON *json_alert)
{

#ifdef WIN32
    int queue_fd = 0;
#else
    int queue_fd = data->queue;
#endif

    char *msg = cJSON_PrintUnformatted(json_alert);
    mdebug2("Sending event: %s",msg);

    if (wm_sendmsg(data->msg_delay, queue_fd, msg,WM_SCA_STAMP, SCA_MQ) < 0) {
        merror(QUEUE_ERROR, DEFAULTQUEUE, strerror(errno));

        if(data->queue >= 0){
            close(data->queue);
        }

        if ((data->queue = StartMQ(DEFAULTQPATH, WRITE)) < 0) {
            mwarn("Can't connect to queue.");
        } else {
            if(wm_sendmsg(data->msg_delay, data->queue, msg,WM_SCA_STAMP, SCA_MQ) < 0) {
                merror(QUEUE_ERROR, DEFAULTQUEUE, strerror(errno));
                close(data->queue);
            }
        }
    }

    os_free(msg);

    return (0);
}

static void wm_sca_send_policies_scanned(wm_sca_t * data) {
    cJSON *policies_obj = cJSON_CreateObject();
    cJSON *policies = cJSON_CreateArray();

    int i;
    if(data->profile) {
        for(i = 0; data->profile[i]; i++) {
            if(data->profile[i]->enabled) {
                cJSON_AddStringToObject(policies,"policy",data->profile[i]->policy_id);
            }
        }
    }

    cJSON_AddStringToObject(policies_obj, "type", "policies");
    cJSON_AddItemToObject(policies_obj,"policies",policies);

    mdebug2("Sending scanned policies.");
    wm_sca_send_alert(data,policies_obj);
    cJSON_Delete(policies_obj);
}

static int wm_sca_start(wm_sca_t * data) {

    int status = 0;
    time_t time_start = 0;
    time_t time_sleep = 0;

    if (!data->scan_on_start) {
        time_start = time(NULL);

        if (data->scan_day) {
            do {
                status = check_day_to_scan(data->scan_day, data->scan_time);
                if (status == 0) {
                    time_sleep = get_time_to_hour(data->scan_time);
                } else {
                    wm_delay(1000); // Sleep one second to avoid an infinite loop
                    time_sleep = get_time_to_hour("00:00");
                }

                mdebug2("Sleeping for %d seconds", (int)time_sleep);
                wm_delay(1000 * time_sleep);

            } while (status < 0);

        } else if (data->scan_wday >= 0) {

            time_sleep = get_time_to_day(data->scan_wday, data->scan_time);
            minfo("Waiting for turn to evaluate.");
            mdebug2("Sleeping for %d seconds", (int)time_sleep);
            wm_delay(1000 * time_sleep);

        } else if (data->scan_time) {

            time_sleep = get_time_to_hour(data->scan_time);
            minfo("Waiting for turn to evaluate.");
            mdebug2("Sleeping for %d seconds", (int)time_sleep);
            wm_delay(1000 * time_sleep);

        } else if (data->next_time == 0 || data->next_time > time_start) {

            // On first run, take into account the interval of time specified
            time_sleep = data->next_time == 0 ?
                         (time_t)data->interval :
                         data->next_time - time_start;

            minfo("Waiting for turn to evaluate.");
            mdebug2("Sleeping for %ld seconds", (long)time_sleep);
            wm_delay(1000 * time_sleep);

        }
    }

    while(1) {
        // Get time and execute
        time_start = time(NULL);

        minfo("Starting Security Configuration Assessment scan.");

        /* Do scan for every policy file */
        wm_sca_read_files(data);

        /* Send policies scanned for database purge on manager side */
        wm_sca_send_policies_scanned(data);

        wm_delay(1000); // Avoid infinite loop when execution fails
        time_sleep = time(NULL) - time_start;

        minfo("Security Configuration Assessment scan finished. Duration: %d seconds.", (int)time_sleep);

        if (data->scan_day) {
            int interval = 0, i = 0;
            interval = data->interval / 60;   // interval in num of months

            do {
                status = check_day_to_scan(data->scan_day, data->scan_time);
                if (status == 0) {
                    time_sleep = get_time_to_hour(data->scan_time);
                    i++;
                } else {
                    wm_delay(1000);
                    time_sleep = get_time_to_hour("00:00");     // Sleep until the start of the next day
                }

                mdebug2("Sleeping for %d seconds", (int)time_sleep);
                wm_delay(1000 * time_sleep);

            } while ((status < 0) && (i < interval));

        } else {

            if (data->scan_wday >= 0) {
                time_sleep = get_time_to_day(data->scan_wday, data->scan_time);
                time_sleep += WEEK_SEC * ((data->interval / WEEK_SEC) - 1);
                data->next_time = (time_t)time_sleep + time_start;
            } else if (data->scan_time) {
                time_sleep = get_time_to_hour(data->scan_time);
                time_sleep += DAY_SEC * ((data->interval / DAY_SEC) - 1);
                data->next_time = (time_t)time_sleep + time_start;
            } else if ((time_t)data->interval >= time_sleep) {
                time_sleep = data->interval - time_sleep;
                data->next_time = data->interval + time_start;
            } else {
                merror("Interval overtaken.");
                time_sleep = data->next_time = 0;
            }

            mdebug2("Sleeping for %d seconds", (int)time_sleep);
            wm_delay(1000 * time_sleep);
        }
    }

    return 0;
}

static void wm_sca_read_files(wm_sca_t * data) {
    FILE *fp;
    int i = 0;
    int checks_number = 0;
    static int first_scan = 1;

    /* Read every policy monitoring file */
    if(data->profile){
        for(i = 0; data->profile[i]; i++) {
            if(!data->profile[i]->enabled){
                continue;
            }

            char path[PATH_MAX] = {0};
            OSStore *vars = NULL;
            cJSON * object = NULL;
            cJSON *requirements_array = NULL;
            int cis_db_index = i;

#ifdef WIN32
            if (data->profile[i]->profile[1] && data->profile[i]->profile[2]) {
                if ((data->profile[i]->profile[1] == ':') || (data->profile[i]->profile[0] == '\\' && data->profile[i]->profile[1] == '\\')) {
                    sprintf(path,"%s", data->profile[i]->profile);
                } else{
                    sprintf(path,"%s\\%s",SECURITY_CONFIGURATION_ASSESSMENT_DIR_WIN, data->profile[i]->profile);
                }
            }
#else
            if(data->profile[i]->profile[0] == '/') {
                sprintf(path,"%s", data->profile[i]->profile);
            } else {
                sprintf(path,"%s/%s",DEFAULTDIR SECURITY_CONFIGURATION_ASSESSMENT_DIR, data->profile[i]->profile);
            }
#endif

            fp = fopen(path,"r");

            if(!fp) {
                mwarn("Policy file not found: '%s'. Skipping it.",path);
                goto next;
            }

            /* Yaml parsing */
            yaml_document_t document;

            if (yaml_parse_file(path, &document)) {
                mwarn("Policy file could not be parsed: '%s'. Skipping it.",path);
                goto next;
            }

            if (object = yaml2json(&document,1), !object) {
                mwarn("Transforming yaml to json: '%s'. Skipping it.",path);
                goto next;
            }

            yaml_document_delete(&document);

            cJSON *policy = cJSON_GetObjectItem(object, "policy");
            cJSON *variables = cJSON_GetObjectItem(object, "variables");
            cJSON *profiles = cJSON_GetObjectItem(object, "checks");
            requirements_array = cJSON_CreateArray();
            cJSON *requirements = cJSON_GetObjectItem(object, "requirements");
            cJSON_AddItemReferenceToArray(requirements_array, requirements);

            if(wm_sca_check_policy(policy, profiles)) {
                mwarn("Validating policy file: '%s'. Skipping it.", path);
                goto next;
            }

            if(requirements && wm_sca_check_requirements(requirements)) {
                mwarn("Reading 'requirements' section of file: '%s'. Skipping it.", path);
                goto next;
            }

            if(!data->profile[i]->policy_id) {
                cJSON *id = cJSON_GetObjectItem(policy, "id");
                os_strdup(id->valuestring,data->profile[i]->policy_id);
            }

            if(!profiles){
                mwarn("Reading 'checks' section of file: '%s'. Skipping it.", path);
                goto next;
            }

            vars = OSStore_Create();

            if( wm_sca_get_vars(variables,vars) != 0 ){
                mwarn("Reading 'variables' section of file: '%s'. Skipping it.", path);
                goto next;
            }

            // Set unique ID for each scan
#ifndef WIN32
            int id = os_random();
            if (id < 0)
                id = -id;
#else
            unsigned int id1 = os_random();
            unsigned int id2 = os_random();

            char random_id[OS_MAXSTR];
            snprintf(random_id, OS_MAXSTR - 1, "%u%u", id1, id2);

            int id = atoi(random_id);
            if (id < 0)
                id = -id;
#endif
            int requirements_satisfied = 0;

            if(!requirements) {
                requirements_satisfied = 1;
            }

            /* Check if the file integrity has changed */
            if(last_sha256[cis_db_index]) {
                w_rwlock_rdlock(&dump_rwlock);
                if (strcmp(last_sha256[cis_db_index],"")) {

                    char * integrity_hash_file = wm_sca_hash_integrity_file(path);

                    /* File hash changed, delete table */
                    if(integrity_hash_file && strcmp(integrity_hash_file,last_sha256[cis_db_index])) {
                        OSHash_Free(cis_db[cis_db_index]);
                        cis_db[cis_db_index] = OSHash_Create();

                        if (!cis_db[cis_db_index]) {
                            merror(LIST_ERROR);
                            w_rwlock_unlock(&dump_rwlock);
                            pthread_exit(NULL);
                        }

                        OSHash_SetFreeDataPointer(cis_db[cis_db_index], (void (*)(void *))wm_sca_free_hash_data);

                        os_free(cis_db_for_hash[cis_db_index].elem);
                        os_realloc(cis_db_for_hash[cis_db_index].elem, sizeof(cis_db_info_t *) * (2), cis_db_for_hash[cis_db_index].elem);
                        cis_db_for_hash[cis_db_index].elem[0] = NULL;
                        cis_db_for_hash[cis_db_index].elem[1] = NULL;
                    }

                    os_free(integrity_hash_file);
                }
                w_rwlock_unlock(&dump_rwlock);
            }

            if(requirements) {
                w_rwlock_rdlock(&dump_rwlock);
                if (wm_sca_do_scan(requirements_array,vars,data,id,policy,1,cis_db_index,data->profile[i]->remote,first_scan,&checks_number) == 0) {
                    requirements_satisfied = 1;
                }
                w_rwlock_unlock(&dump_rwlock);
            }

            if(requirements_satisfied) {
                w_rwlock_rdlock(&dump_rwlock);

                time_t time_start = 0;
                time_t time_end = 0;
                time_start = time(NULL);

                minfo("Starting evaluation of policy: '%s'", data->profile[i]->profile);

                if (wm_sca_do_scan(profiles,vars,data,id,policy,0,cis_db_index,data->profile[i]->remote,first_scan,&checks_number) != 0) {
                    merror("Evaluating the policy file: '%s. Set debug mode for more detailed information.", data->profile[i]->profile);
                }
                mdebug1("Calculating hash for scanned results.");
                char * integrity_hash = wm_sca_hash_integrity(cis_db_index);
                mdebug1("Calculating hash for policy file '%s'", data->profile[i]->profile);
                char * integrity_hash_file = wm_sca_hash_integrity_file(path);

                time_end = time(NULL);

                /* Send summary */
                if(integrity_hash && integrity_hash_file) {
                    wm_delay(1000 * data->summary_delay);
                    wm_sca_send_summary(data,id,summary_passed,summary_failed,summary_invalid,policy,time_start,time_end,integrity_hash,integrity_hash_file,first_scan,cis_db_index,checks_number);
                    snprintf(last_sha256[cis_db_index] ,sizeof(os_sha256),"%s",integrity_hash_file);
                }

                os_free(integrity_hash);
                os_free(integrity_hash_file);

                minfo("Evaluation finished for policy '%s'.",data->profile[i]->profile);
                wm_sca_reset_summary();
                
                w_rwlock_unlock(&dump_rwlock);
            } else {
                cJSON *title = cJSON_GetObjectItem(requirements,"title");
                minfo("Skipping policy '%s': '%s'.",data->profile[i]->profile,title->valuestring);
            }

    next:
            if(fp){
                fclose(fp);
            }

            if(object) {
                cJSON_Delete(object);
            }

            if(requirements_array){
                cJSON_Delete(requirements_array);
            }

            if(vars) {
                OSStore_Free(vars);
            }
        }
        first_scan = 0;
    }
}

static int wm_sca_check_policy(const cJSON * const policy, const cJSON * const profiles)
{
    if(!policy) {
        return 1;
    }

    const cJSON * const id = cJSON_GetObjectItem(policy, "id");
    if(!id) {
        mwarn("Field 'id' not found in policy header.");
        return 1;
    }

    if(!id->valuestring){
        mwarn("Invalid format for field 'id'.");
        return 1;
    }

    const cJSON * const name = cJSON_GetObjectItem(policy, "name");
    if(!name) {
        mwarn("Field 'name' not found in policy header.");
        return 1;
    }

    if(!name->valuestring){
        mwarn("Invalid format for field 'name'.");
        return 1;
    }

    const cJSON * const file = cJSON_GetObjectItem(policy, "file");
    if(!file) {
        mwarn("Field 'file' not found in policy header.");
        return 1;
    }

    if(!file->valuestring){
        mwarn("Invalid format for field 'file'.");
        return 1;
    }

    const cJSON * const description = cJSON_GetObjectItem(policy, "description");
    if(!description) {
        mwarn("Field 'description' not found in policy header.");
        return 1;
    }

    if(!description->valuestring) {
        mwarn("Invalid format for field 'description'.");
        return 1;
    }

    // Check for policy rules with duplicated IDs */
    if (!profiles) {
        mwarn("Section 'checks' not found.");
        return 1;
    }

    int *read_id;
    os_calloc(1, sizeof(int), read_id);
    read_id[0] = 0;

    const cJSON *check;
    cJSON_ArrayForEach(check, profiles) {
        const cJSON * const check_id = cJSON_GetObjectItem(check, "id");
        if (check_id == NULL) {
            mwarn("Check ID not found.");
            free(read_id);
            return 1;
        }

        if (check_id->valueint <= 0) {
            // Invalid ID
            mwarn("Invalid check ID: %d", check_id->valueint);
            free(read_id);
            return 1;
        }

        int i;
        for (i = 0; read_id[i] != 0; i++) {
            if (check_id->valueint == read_id[i]) {
                // Duplicated ID
                mwarn("Duplicated check ID: %d", check_id->valueint);
                free(read_id);
                return 1;
            }
        }

        os_realloc(read_id, sizeof(int) * (i + 2), read_id);
        read_id[i] = check_id->valueint;
        read_id[i + 1] = 0;

        const cJSON * const rules = cJSON_GetObjectItem(check, "rules");

        if (rules == NULL) {
            mwarn("Invalid check %d: no rules found.", check_id->valueint);
            free(read_id);
            return 1;
        }

        int rules_n = 0;
        const cJSON *rule;
        cJSON_ArrayForEach(rule, rules) {
            if (!rule->valuestring) {
                mwarn("Invalid check %d: Empty rule.", check_id->valueint);
                free(read_id);
                return 1;
            }

            char *valuestring_ref = rule->valuestring;
            valuestring_ref += 4 * (!strncmp(valuestring_ref, "NOT ", 4) || !strncmp(valuestring_ref, "not ", 4));

            switch (*valuestring_ref) {
                case 'f':
                case 'd':
                case 'p':
                case 'r':
                case 'c':
                    break;
                case '\0':
                    mwarn("Invalid check %d: Empty rule.", check_id->valueint);
                    free(read_id);
                    return 1;
                default:
                    mwarn("Invalid check %d: Invalid rule format.", check_id->valueint);
                    free(read_id);
                    return 1;
            }

            rules_n++;
            if (rules_n > 255) {
                free(read_id);
                mwarn("Invalid check %d: Maximum number of rules is 255.", check_id->valueint);
                return 1;
            }
        }

        if (rules_n == 0) {
            mwarn("Invalid check %d: no rules found.", check_id->valueint);
            free(read_id);
            return 1;
        }
    }

    free(read_id);
    return 0;
}

static int wm_sca_check_requirements(const cJSON * const requirements)
{
    if(!requirements) {
        return 1;
    }

    const cJSON * const title = cJSON_GetObjectItem(requirements, "title");
    if(!title) {
        merror("Field 'title' not found on requirements.");
        return 1;
    }

    if(!title->valuestring){
        merror("Field 'title' must be a string.");
        return 1;
    }

    const cJSON * const description = cJSON_GetObjectItem(requirements, "description");
    if(!description) {
        merror("Field 'description' not found on policy.");
        return 1;
    }

    if(!description->valuestring){
        merror("Field 'description' must be a string.");
        return 1;
    }

    const cJSON * const condition = cJSON_GetObjectItem(requirements, "condition");
    if(!condition) {
        merror("Field 'condition' not found on policy.");
        return 1;
    }

    if(!condition->valuestring){
        merror("Field 'condition' must be a string.");
        return 1;
    }

    const cJSON * const rules = cJSON_GetObjectItem(requirements, "rules");
    if (!rules) {
        merror("Field 'rules' must be present.");
        return 1;
    }

    if (!cJSON_IsArray(rules)) {
        merror("Field 'rules' must be an array");
        return 1;
    }

    return 0;
}

static int wm_sca_check_dir_list(wm_sca_t * const data, char * const dir_list,
    char * const file, char * const pattern, char **reason)
{
    char *f_value_copy;
    os_strdup(dir_list, f_value_copy);
    char *f_value_copy_ref = f_value_copy;
    int found = RETURN_NOT_FOUND;
    char *dir = NULL;
    mdebug2("Exploring directories [%s]", f_value_copy);
    while ((dir = w_strtok_r_str_delim(",", &f_value_copy_ref))) {
        short is_nfs = IsNFS(dir);
        mdebug2("Checking directory '%s' => is_nfs=%d, skip_nfs=%d", dir, is_nfs, data->skip_nfs);
        if(data->skip_nfs && is_nfs == 1) {
            mdebug2("Directory '%s' flagged as NFS and skip_nfs is enabled.", dir);
            if (*reason == NULL) {
                os_malloc(OS_MAXSTR, *reason);
                sprintf(*reason,"Directory '%s' flagged as NFS and skip_nfs is enabled", dir);
            }
            found = RETURN_INVALID;
        } else {
            int check_result;
            if (file == NULL) {
                check_result = wm_sca_check_dir_existence(dir, reason);
            } else {
                check_result = wm_sca_check_dir(dir, file, pattern, reason);
            }

            if (check_result == RETURN_FOUND) {
                found = RETURN_FOUND;
                mdebug2("Found match in directory '%s'", dir);
            } else if (check_result == RETURN_INVALID) {
                found = RETURN_INVALID;
                mdebug2("Check returned not applicable for directory '%s'", dir);
            }
        }

        char _b_msg[OS_SIZE_1024 + 1];
        _b_msg[OS_SIZE_1024] = '\0';
        snprintf(_b_msg, OS_SIZE_1024, " Directory: %s", dir);
        append_msg_to_vm_scat(data, _b_msg);

        if (found == RETURN_FOUND) {
            break;
        }
    }

    os_free(f_value_copy);
    return found;
}

/*
Rules that match always return 1, and the other way arround.

Rule aggregators logic:

##########################################################

ALL:
    r_1 -f -> r:123
    ...
    r_n -f -> r:234

For an ALL to succeed, every rule shall return 1, in other words,

               |  = n -> ALL = RETURN_FOUND
SUM(r_i, 0, n) |
               | != n -> ALL = RETURN_NOT_FOUND

##########################################################

ANY:
    r_1 -f -> r:123
    ...
    r_n -f -> r:234

For an ANY to succeed, a rule shall return 1, in other words,

               | > 0 -> ANY = RETURN_FOUND
SUM(r_i, 0, n) |
               | = 0 -> ANY = RETURN_NOT_FOUND

##########################################################

NONE:
    r_1 -f -> r:123
    ...
    r_n -f -> r:234

For a NONE to succeed, all rules shall return RETURN_NOT_FOUND, in other words,

               |  > 0 -> NONE = RETURN_NOT_FOUND
SUM(r_i, 0, n) |
               |  = 0 -> NONE = RETURN_FOUND

##########################################################

ANY and NONE aggregators are complementary.

*/

static int wm_sca_do_scan(cJSON *profile_check, OSStore *vars, wm_sca_t * data, int id,cJSON *policy,
    int requirements_scan, int cis_db_index, unsigned int remote_policy, int first_scan, int *checks_number)
{
    int type = 0;
    char buf[OS_SIZE_1024 + 2];
    char root_dir[OS_SIZE_1024 + 2];
    char final_file[2048 + 1];
    char *reason = NULL;

    int ret_val = 0;
    OSList *p_list = NULL;

    /* Initialize variables */
    memset(buf, '\0', sizeof(buf));
    memset(root_dir, '\0', sizeof(root_dir));
    memset(final_file, '\0', sizeof(final_file));

#ifdef WIN32
    /* Get Windows rootdir */
    wm_sca_getrootdir(root_dir, sizeof(root_dir) - 1);
    if (root_dir[0] == '\0') {
        merror(INVALID_ROOTDIR);
    }
#endif

    int check_count = 0;
    cJSON *profile = NULL;
    cJSON_ArrayForEach(profile, profile_check) {
        char _check_id_str[50];
        if (requirements_scan) {
            snprintf(_check_id_str, sizeof(_check_id_str), "Requirements check");
        } else {
            const cJSON * const c_id = cJSON_GetObjectItem(profile, "id");
            if (!c_id || !c_id->valueint) {
                merror("Skipping check. Check ID is invalid. Offending check number: %d.", check_count);
                ret_val = 1;
                continue;
            }
            snprintf(_check_id_str, sizeof(_check_id_str), "id: %d", c_id->valueint);
        }

        const cJSON * const c_title = cJSON_GetObjectItem(profile, "title");
        if (!c_title || !c_title->valuestring) {
            merror("Skipping check with %s: Check name is invalid.", _check_id_str);
            if (requirements_scan) {
                ret_val = 1;
                goto clean_return;
            }
            continue;
        }

        const cJSON * const c_condition = cJSON_GetObjectItem(profile, "condition");
        if (!c_condition || !c_condition->valuestring) {
            merror("Skipping check '%s: %s': Check condition not found.", _check_id_str, c_title->valuestring);
            if (requirements_scan) {
                ret_val = 1;
                goto clean_return;
            }
            continue;
        }

        int condition = 0;
        wm_sca_set_condition(c_condition->valuestring, &condition);

        if (condition == WM_SCA_COND_INV) {
            merror("Skipping check '%s: %s': Check condition (%s) is invalid.",_check_id_str, c_title->valuestring, c_condition->valuestring);
            if (requirements_scan) {
                ret_val = 1;
                goto clean_return;
            }
            continue;
        }

        int g_found = RETURN_NOT_FOUND;
        if (condition & WM_SCA_COND_ANY || (condition & WM_SCA_COND_NON)) {
            // ANY/NONE rules break by finding a match -> break -> success/failure
            g_found = RETURN_NOT_FOUND;
        } else if (condition & WM_SCA_COND_ALL) {
            // ALL rules break by failure -> break -> failure
            g_found = RETURN_FOUND;
        }

        mdebug1("Beginning evaluation of check %s '%s'.", _check_id_str, c_title->valuestring);
        mdebug1("Rule aggregation strategy for this check is '%s'", c_condition->valuestring);
        mdebug2("Initial rule-aggregator value por this type of rule is '%d'.",  g_found);
        mdebug1("Beginning rules evaluation.");

        const cJSON *const rules = cJSON_GetObjectItem(profile, "rules");
        if (!rules) {
            merror("Skipping check %s '%s': No rules found.", _check_id_str, c_title->valuestring);
            if (requirements_scan) {
                ret_val = 1;
                goto clean_return;
            }
            continue;
        }

        char *rule_cp = NULL;
        const cJSON *rule_ref;
        cJSON_ArrayForEach(rule_ref, rules) {
            /* this free is responsible of freeing the copy of the previous rule if
            the loop 'continues', i.e, does not reach the end of its block. */
            os_free(rule_cp);

            if(!rule_ref->valuestring) {
                mdebug1("Field 'rule' must be a string.");
                ret_val = 1;
                goto clean_return;
            }

            mdebug1("Evaluating rule: '%s'", rule_ref->valuestring);

            os_strdup(rule_ref->valuestring, rule_cp);

            char *rule_cp_ref = rule_cp;
            int rule_is_negated = 0;
            if (rule_cp_ref &&
                    (strncmp(rule_cp_ref, "NOT ", 4) == 0 ||
                     strncmp(rule_cp_ref, "not ", 4) == 0))
            {
                mdebug2("Rule is negated");
                rule_is_negated = 1;
                rule_cp_ref += 4;
            }

            /* Get value to look for. char *value is a reference
            to rule_cp memory. Do not release value!  */
            char *value = wm_sca_get_value(rule_cp_ref, &type);

            if (value == NULL) {
                merror("Invalid rule: '%s'. Skipping policy.", rule_ref->valuestring);
                os_free(rule_cp);
                ret_val = 1;
                goto clean_return;
            }

            int found = RETURN_NOT_FOUND;
            /* Check for a file */
            if (type == WM_SCA_TYPE_FILE) {
                char *pattern = wm_sca_get_pattern(value);
                char *file_list = value;

                /* Get any variable */
                if (value[0] == '$') {
                    file_list = (char *) OSStore_Get(vars, value);
                    if (!file_list) {
                        merror("Invalid variable: '%s'. Skipping check.", value);
                        continue;
                    }
                }

            #ifdef WIN32
                else {
                    final_file[0] = '\0';
                    final_file[sizeof(final_file) - 1] = '\0';
                    if (value[0] == '\\') {
                        snprintf(final_file, sizeof(final_file) - 2, "%s%s", root_dir, value);
                    } else {
                        ExpandEnvironmentStrings(value, final_file, sizeof(final_file) - 2);
                    }
                    file_list = final_file;
                }
            #endif

                const int result = wm_sca_check_file_list(file_list, pattern, &reason);
                if (result == RETURN_FOUND || result == RETURN_INVALID) {
                    found = result;
                }

                char _b_msg[OS_SIZE_1024 + 1];
                _b_msg[OS_SIZE_1024] = '\0';
                snprintf(_b_msg, OS_SIZE_1024, " File: %s", file_list);
                append_msg_to_vm_scat(data, _b_msg);
            }
            /* Check for a command */
            else if (type == WM_SCA_TYPE_COMMAND) {
                char *pattern = wm_sca_get_pattern(value);
                char *f_value = value;

                if (!data->remote_commands && remote_policy) {
                    mwarn("Ignoring check for policy '%s'. The internal option 'sca.remote_commands' is disabled.", cJSON_GetObjectItem(policy, "name")->valuestring);
                    if (reason == NULL) {
                        os_malloc(OS_MAXSTR, reason);
                        sprintf(reason,"Ignoring check for running command '%s'. The internal option 'sca.remote_commands' is disabled", f_value);
                    }
                    found = RETURN_INVALID;
                } else {
                    /* Get any variable */
                    if (value[0] == '$') {
                        f_value = (char *) OSStore_Get(vars, value);
                        if (!f_value) {
                            merror("Invalid variable: '%s'. Skipping check.", value);
                            continue;
                        }
                    }

                    mdebug2("Running command: '%s'.", f_value);
                    const int val = wm_sca_read_command(f_value, pattern, data, &reason);
                    if (val == RETURN_FOUND) {
                        mdebug2("Command output matched.");
                        found = RETURN_FOUND;
                    } else if (val == RETURN_INVALID){
                        mdebug2("Command output did not match.");
                        found = RETURN_INVALID;
                    }
                }

                char _b_msg[OS_SIZE_1024 + 1];
                _b_msg[OS_SIZE_1024] = '\0';
                snprintf(_b_msg, OS_SIZE_1024, " Command: %s", f_value);
                append_msg_to_vm_scat(data, _b_msg);
            }

        #ifdef WIN32
            /* Check for a registry entry */
            else if (type == WM_SCA_TYPE_REGISTRY) {
                found = wm_check_registry_entry(value, &reason);

                char _b_msg[OS_SIZE_1024 + 1];
                _b_msg[OS_SIZE_1024] = '\0';
                snprintf(_b_msg, OS_SIZE_1024, " Registry: %s", value);
                append_msg_to_vm_scat(data, _b_msg);
            }
        #endif
            /* Check for a directory */
            else if (type == WM_SCA_TYPE_DIR) {
                mdebug2("Processing directory rule '%s'", value);
                char * const file = wm_sca_get_pattern(value);
                char *f_value = value;

                /* Get any variable */
                if (value[0] == '$') {
                    f_value = (char *) OSStore_Get(vars, value);
                    if (!f_value) {
                        merror("Invalid variable: '%s'. Skipping check.", value);
                        continue;
                    }
                }

                char * const pattern = wm_sca_get_pattern(file);
                found = wm_sca_check_dir_list(data, f_value, file, pattern, &reason);
                mdebug2("Check directory rule result: %d", found);
            }
            /* Check for a process */
            else if (type == WM_SCA_TYPE_PROCESS) {
                if (!p_list) {
                    p_list = w_os_get_process_list();
                }

                mdebug2("Checking process: '%s'", value);
                if (wm_sca_check_process_is_running(p_list, value)) {
                    mdebug2("Process found.");
                    found = RETURN_FOUND;
                } else {
                    mdebug2("Process not found.");
                }
                char _b_msg[OS_SIZE_1024 + 1];
                _b_msg[OS_SIZE_1024] = '\0';
                snprintf(_b_msg, OS_SIZE_1024, " Process: %s", value);
                append_msg_to_vm_scat(data, _b_msg);
            }

            /* Rule result processing */

            if (found != RETURN_INVALID) {
                found = rule_is_negated ^ found;
            }

            mdebug1("Result for rule '%s': %d", rule_ref->valuestring, found);

            if (((condition & WM_SCA_COND_ALL) && found == RETURN_NOT_FOUND) ||
                ((condition & WM_SCA_COND_ANY) && found == RETURN_FOUND) ||
                ((condition & WM_SCA_COND_NON) && found == RETURN_FOUND))
            {
                g_found = found;
                mdebug1("Breaking from rule aggregator '%s' with found = %d.", c_condition->valuestring, g_found);
                break;
            } else if (found == RETURN_INVALID) {
                /*  Rules that agreggate by ANY are the only that can recover from an INVALID,
                    and should keep it, should all their checks are INVALID */
                g_found = found;
                mdebug1("Rule evaluation returned INVALID. Continuing");
            }
        }

        if ((condition & WM_SCA_COND_NON) && g_found != RETURN_INVALID) {
            g_found = !g_found;
        }

        mdebug1("Result for check %s '%s' -> %d", _check_id_str, c_title->valuestring, g_found);

        if (g_found != RETURN_INVALID) {
            os_free(reason);
        }

        /* if the loop breaks, rule_cp shall be released.
            Also frees the the memory reserved on the last iteration */
        os_free(rule_cp);

        /* Determine if requirements are satisfied */
        if (requirements_scan) {
            //wm_sca_reset_summary();
            /*  return value for requirement scans is the inverse of the result,
                unless the result is INVALID */
            ret_val = g_found == RETURN_INVALID ? 1 : !g_found;
            int i;
            for (i=0; data->alert_msg[i]; i++){
                free(data->alert_msg[i]);
                data->alert_msg[i] = NULL;
            }
            goto clean_return;
        }

        /* Event construction */
        const char failed[] = "failed";
        const char passed[] = "passed";
        const char invalid[] = ""; //NOT AN ERROR!
        const char *message_ref = NULL;

        if (g_found == RETURN_NOT_FOUND) {
            wm_sca_summary_increment_failed();
            message_ref = failed;
        } else if (g_found == RETURN_FOUND) {
            wm_sca_summary_increment_passed();
            message_ref = passed;
        } else {
            wm_sca_summary_increment_invalid();
            message_ref = invalid;
        }

        cJSON *event = wm_sca_build_event(profile, policy, data->alert_msg, id, message_ref, reason);
        if (event) {
            /* Alert if necessary */
            if(!cis_db_for_hash[cis_db_index].elem[check_count]) {
                os_realloc(cis_db_for_hash[cis_db_index].elem, sizeof(cis_db_info_t *) * (check_count + 2), cis_db_for_hash[cis_db_index].elem);
                cis_db_for_hash[cis_db_index].elem[check_count] = NULL;
                cis_db_for_hash[cis_db_index].elem[check_count + 1] = NULL;
            }

            if (wm_sca_check_hash(cis_db[cis_db_index], message_ref, profile, event, check_count, cis_db_index) && !first_scan) {
                wm_sca_send_event_check(data,event);
            }

            check_count++;

            cJSON_Delete(event);
        } else {
            merror("Error constructing event for check: %s. Set debug mode for more information.", c_title->valuestring);
            ret_val = 1;
        }

        int i;
        for (i=0; data->alert_msg[i]; i++){
            free(data->alert_msg[i]);
            data->alert_msg[i] = NULL;
        }

        os_free(reason);
    }

    *checks_number = check_count;

/* Clean up memory */
clean_return:
    os_free(reason);
    w_del_plist(p_list);

    return ret_val;
}

static void wm_sca_set_condition(const char * const c_cond, int *condition)
{
    if (strcmp(c_cond, "all") == 0) {
        *condition |= WM_SCA_COND_ALL;
    } else if (strcmp(c_cond, "any") == 0) {
        *condition |= WM_SCA_COND_ANY;
    } else if (strcmp(c_cond, "none") == 0) {
        *condition |= WM_SCA_COND_NON;
    } else if (strcmp(c_cond, "any required") == 0) {
        *condition |= WM_SCA_COND_ANY;
        minfo("Modifier 'required' is deprecated. Defaults to 'any'.");
    } else if (strcmp(c_cond, "all required") == 0) {
        *condition |= WM_SCA_COND_ALL;
        minfo("Modifier 'required' is deprecated. Defaults to 'all'.");
    } else {
        *condition = WM_SCA_COND_INV;
    }
}

static int wm_sca_get_vars(const cJSON * const variables, OSStore * const vars)
{
    const cJSON *variable;
    cJSON_ArrayForEach (variable, variables) {
        /* If not a variable, return 0 */
        if (*variable->string != '$') {
            merror("Invalid variable: '%s'. Skipping check.", variable->string);
            return 0;
        }

        /* Remove semicolon from the end */
        char *tmp = strchr(variable->valuestring, ';');
        if (tmp) {
            *tmp = '\0';
        } else {
            return -1;
        }

        char *var_value;
        os_strdup(variable->valuestring,var_value);
        OSStore_Put(vars, variable->string, var_value);
    }

    return 0;
}

static char *wm_sca_get_value(char *buf, int *type)
{
    /* Zero type before using it to make sure return is valid
     * in case of error.
     */
    *type = 0;

    char *value = strchr(buf, ':');
    if (value == NULL) {
        return NULL;
    }

    *value = '\0';
    value++;

    char *tmp_str = strchr(value, ';');
    if (tmp_str != NULL) {
        *tmp_str = '\0';
    }

    /* Get types - removing negate flag (using later) */
    if (*buf == '!') {
        buf++;
    }

    if (strcmp(buf, "f") == 0) {
        *type = WM_SCA_TYPE_FILE;
    } else if (strcmp(buf, "r") == 0) {
        *type = WM_SCA_TYPE_REGISTRY;
    } else if (strcmp(buf, "p") == 0) {
        *type = WM_SCA_TYPE_PROCESS;
    } else if (strcmp(buf, "d") == 0) {
        *type = WM_SCA_TYPE_DIR;
    } else if (strcmp(buf, "c") == 0) {
        *type = WM_SCA_TYPE_COMMAND;
    } else {
        return NULL;
    }

    return value;
}

static char *wm_sca_get_pattern(char *value)
{
    if (value == NULL) {
        return NULL;
    }

    while (*value != '\0') {
        if ((*value == ' ') && (value[1] == '-') &&
                (value[2] == '>') && (value[3] == ' ')) {
            *value = '\0';
            value += 4;

            return (value);
        }
        value++;
    }

    return (NULL);
}

static int wm_sca_check_file_existence(const char * const file, char **reason)
{
    struct stat statbuf;
    const int lstat_ret = lstat(file, &statbuf);
    const int lstat_errno = errno;

    if (lstat_ret == -1) {
        if (lstat_errno == ENOENT) {
            mdebug2("FILE_EXISTS(%s) -> RETURN_NOT_FOUND: %s", file, strerror(lstat_errno));
            return RETURN_NOT_FOUND;
        }
        if (*reason == NULL) {
            os_malloc(OS_MAXSTR, *reason);
            sprintf(*reason, "Could not open '%s': %s", file, strerror(lstat_errno));
        }
        mdebug2("FILE_EXISTS(%s) -> RETURN_INVALID: %s", file, strerror(lstat_errno));
        return RETURN_INVALID;
    }

    if (S_ISREG(statbuf.st_mode)) {
        mdebug2("FILE_EXISTS(%s) -> RETURN_FOUND", file);
        return RETURN_FOUND;
    }

    if (*reason == NULL) {
        os_malloc(OS_MAXSTR, *reason);
        sprintf(*reason, "FILE_EXISTS(%s) -> RETURN_INVALID: Not a regular file.", file);
    }

    mdebug2("FILE_EXISTS(%s) -> RETURN_INVALID: Not a regular file.", file);
    return RETURN_INVALID;
}

static int wm_sca_check_file_contents(const char * const file, const char * const pattern, char **reason)
{
    mdebug2("Checking contents of file '%s' against pattern '%s'", file, pattern);
    FILE *fp = fopen(file, "r");
    const int fopen_errno = errno;
    if (!fp) {
        if (*reason == NULL) {
            os_malloc(OS_MAXSTR, *reason);
            sprintf(*reason, "Could not open file '%s': %s", file, strerror(fopen_errno));
        }
        mdebug2("Could not open file '%s': %s", file, strerror(fopen_errno));
        return RETURN_INVALID;
    }

    int result = RETURN_NOT_FOUND;
    char buf[OS_SIZE_2048 + 1];
    while (fgets(buf, OS_SIZE_2048, fp) != NULL) {
        os_trimcrlf(buf);
        mdebug2("(%s)(%s) -> %d", pattern, *buf != '\0' ? buf : "EMPTY_LINE" , result);

        result = wm_sca_pt_matches(buf, pattern);

        if (result) {
            mdebug2("Match found. Skipping the rest.");
            break;
        }
    }

    fclose(fp);
    mdebug2("Result for (%s)(%s) -> %d", pattern, file, result);
    return result;
}

static int wm_sca_check_file_list(const char * const file_list, char * const pattern, char **reason)
{
    if (pattern) {
        return wm_sca_check_file_list_for_contents(file_list, pattern, reason);
    }

    return wm_sca_check_file_list_for_existence(file_list, reason);
}

static int wm_sca_check_file_list_for_existence(const char * const file_list, char **reason)
{
    mdebug1("Checking file list '%s' for existence", file_list);

    if (!file_list) {
        return RETURN_NOT_FOUND;
    }

    int result_accumulator = RETURN_NOT_FOUND;
    char *file_list_copy = NULL;
    os_strdup(file_list, file_list_copy);
    char *file_list_ref = file_list_copy;
    char *file = NULL;
    while ((file = strtok_r(file_list_ref, ",", &file_list_ref))) {
        const int file_check_result = wm_sca_check_file_existence(file, reason);
        if (file_check_result == RETURN_FOUND) {
            result_accumulator = RETURN_FOUND;
            mdebug2("File '%s' found. Skipping the rest.", file);
            break;
        }

        if (file_check_result == RETURN_INVALID) {
            result_accumulator = RETURN_INVALID;
             mdebug2("Could not open file '%s'. Continuing", file);
        } else {
             mdebug2("File '%s' does not exists. Continuing", file);
        }
    }

    mdebug1("Result for FILES_EXIST(%s) -> %d", file_list, result_accumulator);

    os_free(file_list_copy);
    return result_accumulator;
}

static int wm_sca_check_file_list_for_contents(const char * const file_list, char * const pattern, char **reason)
{
    mdebug1("Checking file list '%s' with '%s'", file_list, pattern);

    if (!file_list) {
        return RETURN_NOT_FOUND;
    }

    int result_accumulator = RETURN_NOT_FOUND;
    char *file_list_copy = NULL;
    os_strdup(file_list, file_list_copy);
    char *file_list_ref = file_list_copy;
    char *file = NULL;
    while ((file = strtok_r(file_list_ref, ",", &file_list_ref))) {
        const int existence_check_result = wm_sca_check_file_existence(file, reason);
        if (existence_check_result != RETURN_FOUND) {
            /* a file that does not exist produces an INVALID check */
            result_accumulator = RETURN_INVALID;
            if (*reason == NULL) {
                os_malloc(OS_MAXSTR, *reason);
                sprintf(*reason, "Could not open file '%s'",  file);
            }
            mdebug2("Could not open file '%s'. Skipping", file);
            continue;
        }

        const int contents_check_result = wm_sca_check_file_contents(file, pattern, reason);
        if (contents_check_result == RETURN_FOUND) {
            result_accumulator = RETURN_FOUND;
            mdebug2("Match found in '%s'. Skipping the rest.", file);
            break;
        }

        if (contents_check_result == RETURN_INVALID) {
            mdebug2("Check was invalid in file '%s'. Continuing", file);
            result_accumulator = RETURN_INVALID;
        } else {
            mdebug2("Match not found in file '%s'. Continuing", file);
        }
    }

    mdebug1("Result for (%s)(%s) -> %d", pattern, file_list, result_accumulator);

    os_free(file_list_copy);
    return result_accumulator;
}

static int wm_sca_read_command(char *command, char *pattern, wm_sca_t * data, char **reason)
{
    if (command == NULL) {
        mdebug1("No Command specified Returning.");
        return RETURN_NOT_FOUND;
    }

    if (pattern == NULL) {
        mdebug1("No pattern given. Returning FOUND");
        return RETURN_FOUND;
    }

    mdebug1("Executing command '%s', and testing output with pattern '%s'", command, pattern);
    char *cmd_output = NULL;
    int result_code;

    switch( wm_exec(command,&cmd_output,&result_code,data->commands_timeout,NULL)) {
    case 0:
        if (result_code > 0) {
            mdebug1("Command (%s) returned code %d.", command, result_code);
            if (*reason == NULL){
                os_malloc(OS_MAXSTR, *reason);
                if (result_code == EXECVE_ERROR) {
                    mdebug1("Invalid path or permissions running command '%s'",command);
                    sprintf(*reason, "Invalid path or permissions running command '%s'",command);
                } else {
                    mdebug1("Internal error running command '%s'", command);
                    sprintf(*reason, "Internal error running command '%s'", command);
                }
            }
            os_free(cmd_output);
            return RETURN_INVALID;
        }
        break;
    case 1:
        if (*reason == NULL) {
            os_malloc(OS_MAXSTR, *reason);
            mdebug1("Timeout overtaken running command '%s'", command);
            sprintf(*reason, "Timeout overtaken running command '%s'", command);
        }
        return RETURN_INVALID;
    default:
        mdebug1("Command (%s) returned code %d.", command, result_code);

        if (*reason == NULL) {
            os_malloc(OS_MAXSTR, *reason);
            mdebug1("Failed to run command '%s'", command);
            sprintf(*reason, "Failed to run command '%s'", command);
        }
        return RETURN_INVALID;
    }

    if(!cmd_output) {
        mdebug2("Command yielded no output. Returning.");
        return RETURN_NOT_FOUND;
    }

    char **output_line;
    output_line = OS_StrBreak('\n', cmd_output, 256);

    if(!output_line) {
        mdebug1("Command output could not be processed. Output dump:\n%s", cmd_output);
        os_free(cmd_output);
        return RETURN_NOT_FOUND;
    }

    os_free(cmd_output);

    int i;
    int result = RETURN_NOT_FOUND;
    for (i=0; output_line[i] != NULL; i++) {
        char *buf = output_line[i];
        os_trimcrlf(buf);
        result = wm_sca_pt_matches(buf, pattern);
        if (result == RETURN_FOUND){
            break;
        }
    }

    free_strarray(output_line);
    mdebug2("Result for (%s)(%s) -> %d", pattern, command, result);
    return result;
}

int wm_sca_test_positive_minterm(char * const minterm, const char * const str)
{
    const char *pattern_ref = minterm;
    if (strncasecmp(pattern_ref, "=:", 2) == 0) {
        pattern_ref += 2;
        if (strcasecmp(pattern_ref, str) == 0) {
            return RETURN_FOUND;
        }
    } else if (strncasecmp(pattern_ref, "r:", 2) == 0) {
        pattern_ref += 2;
        if (OS_Regex(pattern_ref, str)) {
            return RETURN_FOUND;
        }
    } else if (strncasecmp(pattern_ref, "<:", 2) == 0) {
        pattern_ref += 2;
        if (strcmp(pattern_ref, str) < 0) {
            return RETURN_FOUND;
        }
    } else if (strncasecmp(pattern_ref, ">:", 2) == 0) {
        pattern_ref += 2;
        if (strcmp(pattern_ref, str) > 0) {
            return RETURN_FOUND;
        }
    } else {
#ifdef WIN32
        char final_file[2048 + 1];

        /* Try to get Windows variable */
        if (*pattern_ref == '%') {
            final_file[0] = '\0';
            final_file[2048] = '\0';
            ExpandEnvironmentStrings(pattern_ref, final_file, 2047);
        } else {
            strncpy(final_file, pattern_ref, 2047);
        }

        /* Compare against the expanded variable */
        if (strcasecmp(final_file, str) == 0) {
            return RETURN_FOUND;
        }
#else
        if (strcasecmp(pattern_ref, str) == 0) {
            return RETURN_FOUND;
        }
#endif
    }
    return RETURN_NOT_FOUND;
}

int wm_sca_pt_matches(const char * const str, const char * const pattern)
{
    if (str == NULL) {
        return 0;
    }

    char *pattern_copy = NULL;
    os_strdup(pattern, pattern_copy);
    char *pattern_copy_ref = pattern_copy;
    char *minterm = NULL;
    int test_result = RETURN_FOUND;

    while ((minterm = w_strtok_r_str_delim(" && ", &pattern_copy_ref))) {
        int negated = 0;
        if ((*minterm) == '!'){
            minterm++;
            negated = 1;
        }
        const int minterm_result = negated ^ wm_sca_test_positive_minterm (minterm, str);
        test_result *= minterm_result;
        mdebug2("Testing minterm (%s%s)(%s) -> %d", negated ? "!" : "", minterm, *str != '\0' ? str : "EMPTY_LINE", minterm_result);
    }

    mdebug2("Pattern test result: (%s)(%s) -> %d", pattern, str, test_result);
    os_free(pattern_copy);
    return test_result;
}

static int wm_sca_check_dir_existence(const char * const dir, char **reason)
{
    DIR *dp = opendir(dir);
    const int open_dir_errno = errno;
    if (dp) {
        mdebug2("DIR_EXISTS(%s) -> RETURN_FOUND", dir);
        closedir(dp);
        return RETURN_FOUND;
    }

    if (open_dir_errno == ENOENT) {
        mdebug2("DIR_EXISTS(%s) -> RETURN_NOT_FOUND. Reason: %s", dir, strerror(open_dir_errno));
        return RETURN_NOT_FOUND;
    }

    if (*reason == NULL) {
        os_malloc(OS_MAXSTR, *reason);
        sprintf(*reason, "Could not check directory existence for '%s': %s", dir, strerror(open_dir_errno));
    }
    mdebug2("DIR_EXISTS(%s) -> RETURN_INVALID. Reason: %s", dir, strerror(open_dir_errno));
    return RETURN_INVALID;
}

static int wm_sca_check_dir(const char * const dir, const char * const file, char * const pattern, char **reason)
{
    mdebug2("Checking directory '%s'%s%s%s%s", dir,
        file ? " -> "  : "", file ? file : "",
        pattern ? " -> " : "", pattern ? pattern: "");

    DIR *dp = opendir(dir);
    if (!dp) {
        const int open_dir_errno = errno;
        if (*reason == NULL) {
            os_malloc(OS_MAXSTR, *reason);
            sprintf(*reason, "Could not open '%s': %s", dir, strerror(open_dir_errno));
        }
        mdebug2("Could not open '%s': %s", dir, strerror(open_dir_errno));
        return RETURN_INVALID;
    }

    int result_accumulator = RETURN_NOT_FOUND;
    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        /* Ignore . and ..  */
        if ((strcmp(entry->d_name, ".") == 0) || (strcmp(entry->d_name, "..") == 0)) {
            continue;
        }

        /* Create new file + path string */
        char f_name[PATH_MAX + 2];
        f_name[PATH_MAX + 1] = '\0';
        snprintf(f_name, PATH_MAX + 1, "%s/%s", dir, entry->d_name);

        mdebug2("Considering directory entry '%s'", f_name);

        int result;
        struct stat statbuf_local;
        if (lstat(f_name, &statbuf_local) != 0) {
            mdebug2("Cannot check directory entry '%s'", f_name);
            if (*reason == NULL){
                os_malloc(OS_MAXSTR, *reason);
                sprintf(*reason, "Cannot check directory entry '%s", f_name);
            }
            result_accumulator = RETURN_INVALID;
            continue;
        }

        if (S_ISDIR(statbuf_local.st_mode)) {
            result = wm_sca_check_dir(f_name, file, pattern, reason);
        } else if (((strncasecmp(file, "r:", 2) == 0) && OS_Regex(file + 2, entry->d_name))
                || OS_Match2(file, entry->d_name))
        {
            result = wm_sca_check_file_list(f_name, pattern, reason);
        } else {
            mdebug2("Skipping directory entry '%s'", f_name);
            continue;
        }

        mdebug2("Result for entry '%s': %d", f_name, result);

        if (result == RETURN_FOUND) {
            mdebug2("Match found in '%s', skipping the rest.", f_name);
            result_accumulator = RETURN_FOUND;
            break;
        } else if (result == RETURN_INVALID) {
            result_accumulator = RETURN_INVALID;
        }
    }

    closedir(dp);
    mdebug2("Check result for dir '%s': %d", dir, result_accumulator);
    return result_accumulator;
}

static int wm_sca_check_process_is_running(OSList *p_list, char *value)
{
    if (p_list == NULL) {
        return RETURN_INVALID;
    }

    if (!value) {
        return RETURN_NOT_FOUND;
    }

    OSListNode *l_node = OSList_GetFirstNode(p_list);
    while (l_node) {
        W_Proc_Info *pinfo = (W_Proc_Info *)l_node->data;
        /* Check if value matches */
        if (wm_sca_pt_matches(pinfo->p_path, value)) {
            return RETURN_FOUND;
        }

        l_node = OSList_GetNextNode(p_list);
    }

    return RETURN_NOT_FOUND;
}

// Destroy data
void wm_sca_destroy(wm_sca_t * data)
{
    os_free(data);
}

#ifdef WIN32

static int wm_check_registry_entry(char * const value, char **reason)
{
    char * const entry = wm_sca_get_pattern(value);
    char * const pattern = wm_sca_get_pattern(entry);
    return wm_sca_is_registry(value, entry, pattern, reason);
}

static int wm_sca_is_registry(char *entry_name, char *reg_option, char *reg_value, char **reason)
{
    char *rk = wm_sca_os_winreg_getkey(entry_name);

    if (wm_sca_sub_tree == NULL || rk == NULL) {
         if (*reason == NULL) {
            os_malloc(OS_MAXSTR, *reason);
            sprintf(*reason, "Invalid registry entry: '%s'", entry_name);
        }

        merror("Invalid registry entry: '%s'", entry_name);
        return RETURN_INVALID;
    }

    int returned_value_64 = wm_sca_test_key(rk, entry_name, KEY_WOW64_64KEY, reg_option, reg_value, reason);

    int returned_value_32 = RETURN_NOT_FOUND;
    if (returned_value_64 != RETURN_FOUND) {
        returned_value_32 = wm_sca_test_key(rk, entry_name, KEY_WOW64_32KEY, reg_option, reg_value, reason);
    }

    int ret_value = RETURN_NOT_FOUND;
    if (returned_value_32 == RETURN_INVALID && returned_value_64 == RETURN_INVALID) {
        ret_value = RETURN_INVALID;
    } else if (returned_value_32 == RETURN_FOUND || returned_value_64 == RETURN_FOUND) {
        ret_value = RETURN_FOUND;
    }

    return ret_value;
}

static char *wm_sca_os_winreg_getkey(char *reg_entry)
{
    char *ret = NULL;
    char *tmp_str;

    /* Get only the sub tree first */
    tmp_str = strchr(reg_entry, '\\');
    if (tmp_str) {
        *tmp_str = '\0';
        ret = tmp_str + 1;
    }

    /* Set sub tree */
    if ((strcmp(reg_entry, "HKEY_LOCAL_MACHINE") == 0) ||
            (strcmp(reg_entry, "HKLM") == 0)) {
        wm_sca_sub_tree = HKEY_LOCAL_MACHINE;
    } else if (strcmp(reg_entry, "HKEY_CLASSES_ROOT") == 0) {
        wm_sca_sub_tree = HKEY_CLASSES_ROOT;
    } else if (strcmp(reg_entry, "HKEY_CURRENT_CONFIG") == 0) {
        wm_sca_sub_tree = HKEY_CURRENT_CONFIG;
    } else if (strcmp(reg_entry, "HKEY_USERS") == 0) {
        wm_sca_sub_tree = HKEY_USERS;
    } else if ((strcmp(reg_entry, "HKCU") == 0) ||
               (strcmp(reg_entry, "HKEY_CURRENT_USER") == 0)) {
        wm_sca_sub_tree = HKEY_CURRENT_USER;
    } else {
        /* Set sub tree to null */
        wm_sca_sub_tree = NULL;

        /* Return tmp_str to the previous value */
        if (tmp_str && (*tmp_str == '\0')) {
            *tmp_str = '\\';
        }
        return (NULL);
    }

    /* Check if ret has nothing else */
    if (ret && (*ret == '\0')) {
        ret = NULL;
    }

    /* Fixing tmp_str and the real name of the registry */
    if (tmp_str && (*tmp_str == '\0')) {
        *tmp_str = '\\';
    }

    return (ret);
}

static int wm_sca_test_key(char *subkey, char *full_key_name, unsigned long arch,
                         char *reg_option, char *reg_value, char **reason)
{
    mdebug2("Checking '%s' in the %dBIT subsystem.", full_key_name, arch == KEY_WOW64_64KEY ? 64 : 32);

    HKEY oshkey;
    LSTATUS err = RegOpenKeyEx(wm_sca_sub_tree, subkey, 0, KEY_READ | arch, &oshkey);
    if (err == ERROR_ACCESS_DENIED) {
        if (*reason == NULL) {
            os_malloc(OS_MAXSTR, *reason);
            sprintf(*reason, "Access denied for registry '%s'", full_key_name);
        }
        merror("Access denied for registry '%s'", full_key_name);
        return RETURN_INVALID;
    } else if (err != ERROR_SUCCESS) {
        char error_msg[OS_SIZE_1024 + 1];
        error_msg[OS_SIZE_1024] = '\0';
        FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS
                    | FORMAT_MESSAGE_MAX_WIDTH_MASK,
                    NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                    (LPTSTR) &error_msg, OS_SIZE_1024, NULL);

        mdebug2("Unable to read registry '%s': %s", full_key_name, error_msg);

        /* If registry not found and no key is requested -> return RETURN_NOT_FOUND */
        if (!reg_option) {
            mdebug2("Registry '%s' not found.", full_key_name);
            return RETURN_NOT_FOUND;
        }

        if (*reason == NULL){
            os_malloc(OS_MAXSTR, *reason);
            sprintf(*reason, "Unable to read registry '%s' (%s)", full_key_name, error_msg);
        }
        return RETURN_INVALID;
    }

    /* If the key does exists, a test for existence succeeds  */
    int ret_val = RETURN_FOUND;

    /* If option is set, set test_result as the value of query key */
    if (reg_option) {
        ret_val = wm_sca_winreg_querykey(oshkey, full_key_name, reg_option, reg_value, reason);
    }

    RegCloseKey(oshkey);
    return ret_val;
}

static int wm_sca_winreg_querykey(HKEY hKey, const char *full_key_name, char *reg_option, char *reg_value, char **reason)
{
    int rc;
    DWORD i, j;

    /* QueryInfo and EnumKey variables */
    TCHAR class_name_b[MAX_PATH + 1];
    DWORD class_name_s = MAX_PATH;

    /* Number of sub keys */
    DWORD subkey_count = 0;

    /* Number of values */
    DWORD value_count;

    /* Variables for RegEnumValue */
    TCHAR value_buffer[MAX_VALUE_NAME + 1];
    TCHAR data_buffer[MAX_VALUE_NAME + 1];
    DWORD value_size;
    DWORD data_size;

    /* Data type for RegEnumValue */
    DWORD data_type = 0;

    /* Storage var */
    char var_storage[MAX_VALUE_NAME + 1];

    /* Initialize the memory for some variables */
    class_name_b[0] = '\0';
    class_name_b[MAX_PATH] = '\0';

    /* We use the class_name, subkey_count and the value count */
    rc = RegQueryInfoKey(hKey, class_name_b, &class_name_s, NULL,
                         &subkey_count, NULL, NULL, &value_count,
                         NULL, NULL, NULL, NULL);

    if (rc != ERROR_SUCCESS) {
        char error_msg[OS_SIZE_1024 + 1];
        error_msg[OS_SIZE_1024] = '\0';
        FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS
                    | FORMAT_MESSAGE_MAX_WIDTH_MASK,
                    NULL, rc, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                    (LPTSTR) &error_msg, OS_SIZE_1024, NULL);

        if (*reason == NULL){
            os_malloc(OS_MAXSTR, *reason);
            sprintf(*reason, "Unable to read registry '%s' (%s)", full_key_name, error_msg);
        }

        mdebug2("Unable to read registry '%s': %s", full_key_name, error_msg);
        return RETURN_INVALID;
    }

    /* Get values (if available) */
    if (value_count) {
        char *mt_data;

        /* Clear the values for value_size and data_size */
        value_buffer[MAX_VALUE_NAME] = '\0';
        data_buffer[MAX_VALUE_NAME] = '\0';
        var_storage[MAX_VALUE_NAME] = '\0';

        /* Get each value */
        for (i = 0; i < value_count; i++) {
            value_size = MAX_VALUE_NAME;
            data_size = MAX_VALUE_NAME;

            value_buffer[0] = '\0';
            data_buffer[0] = '\0';
            var_storage[0] = '\0';

            rc = RegEnumValue(hKey, i, value_buffer, &value_size,
                              NULL, &data_type, (LPBYTE)data_buffer, &data_size);

            /* No more values available */
            if (rc != ERROR_SUCCESS && rc != ERROR_NO_MORE_ITEMS) {
                char error_msg[OS_SIZE_1024 + 1];
                error_msg[OS_SIZE_1024] = '\0';
                FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS
                            | FORMAT_MESSAGE_MAX_WIDTH_MASK,
                            NULL, rc, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                            (LPTSTR) &error_msg, OS_SIZE_1024, NULL);

                if (*reason == NULL){
                    os_malloc(OS_MAXSTR, *reason);
                    sprintf(*reason, "Unable to enumerate values of registry '%s' (%s)", full_key_name, error_msg);
                }

                mdebug2("Unable to enumerate values of registry '%s' -> RETURN_INVALID", full_key_name);
                return RETURN_INVALID;
            }

            /* Check if no value name is specified */
            if (value_buffer[0] == '\0') {
                value_buffer[0] = '@';
                value_buffer[1] = '\0';
            }

            /* Check if the entry name matches the reg_option */
            if (strcasecmp(value_buffer, reg_option) != 0) {
                mdebug2("Considering value '%s' -> '%s' != '%s': Skipping value.", full_key_name, value_buffer, reg_option);
                continue;
            }

            mdebug2("Considering value '%s' -> '%s' == '%s': Value found.", full_key_name, value_buffer, reg_option);

            /* If a value is not present and the option matches return found */
            if (!reg_value) {
                mdebug2("No value data especified. Existence check for '%s': 1", full_key_name);
                return RETURN_FOUND;
            }

            /* Write value into a string */
            switch (data_type) {
                    int size_available;

                case REG_SZ:
                case REG_EXPAND_SZ:
                    snprintf(var_storage, MAX_VALUE_NAME, "%s", data_buffer);
                    break;
                case REG_MULTI_SZ:
                    /* Printing multiple strings */
                    size_available = MAX_VALUE_NAME - 3;
                    mt_data = data_buffer;

                    while (*mt_data) {
                        if (size_available > 2) {
                            strncat(var_storage, mt_data, size_available);
                            strncat(var_storage, " ", 2);
                            size_available = MAX_VALUE_NAME -
                                             (strlen(var_storage) + 2);
                        }
                        mt_data += strlen(mt_data) + 1;
                    }

                    break;
                case REG_DWORD:
                    snprintf(var_storage, MAX_VALUE_NAME,
                             "%x", (unsigned int)*data_buffer);
                    break;
                default:
                    size_available = MAX_VALUE_NAME - 2;
                    for (j = 0; j < data_size; j++) {
                        char tmp_c[12];

                        snprintf(tmp_c, 12, "%02x",
                                 (unsigned int)data_buffer[j]);

                        if (size_available > 2) {
                            strncat(var_storage, tmp_c, size_available);
                            size_available = MAX_VALUE_NAME -
                                             (strlen(var_storage) + 2);
                        }
                    }
                    break;
            }

            mdebug2("Checking value data '%s' with rule '%s'", var_storage, reg_value);

            int result = wm_sca_pt_matches(var_storage, reg_value);
            return result;
        }
    }

    if (*reason == NULL && reg_value){
        os_malloc(OS_MAXSTR, *reason);
        sprintf(*reason, "Key '%s' not found for registry '%s'", reg_option, full_key_name);
    }

    return reg_value ? RETURN_INVALID : RETURN_NOT_FOUND;
}

static char *wm_sca_getrootdir(char *root_dir, int dir_size)
{
    char final_file[2048 + 1];
    char *tmp;

    final_file[0] = '\0';
    final_file[2048] = '\0';

    ExpandEnvironmentStrings("%WINDIR%", final_file, 2047);

    tmp = strchr(final_file, '\\');
    if (tmp) {
        *tmp = '\0';
        strncpy(root_dir, final_file, dir_size);
        return (root_dir);
    }

    return (NULL);
}
#endif

static int wm_sca_send_summary(wm_sca_t * data, int scan_id,unsigned int passed, unsigned int failed,unsigned int invalid,cJSON *policy,int start_time,int end_time,char * integrity_hash,char *integrity_hash_file, int first_scan,int id,int checks_number) {

    cJSON *json_summary = cJSON_CreateObject();

    cJSON_AddStringToObject(json_summary, "type", "summary");
    cJSON_AddNumberToObject(json_summary, "scan_id", scan_id);

    /* Policy fields */
    cJSON *name = cJSON_GetObjectItem(policy,"name");
    cJSON *description = cJSON_GetObjectItem(policy,"description");
    cJSON *references = cJSON_GetObjectItem(policy,"references");
    cJSON *policy_id = cJSON_GetObjectItem(policy,"id");
    cJSON *file= cJSON_GetObjectItem(policy,"file");

    cJSON_AddStringToObject(json_summary, "name", name->valuestring);
    cJSON_AddStringToObject(json_summary, "policy_id", policy_id->valuestring);
    cJSON_AddStringToObject(json_summary, "file", file->valuestring);

    if(description) {
        cJSON_AddStringToObject(json_summary, "description", description->valuestring);
    }

    if(references) {
        cJSON *reference;
        char *ref = NULL;

        cJSON_ArrayForEach(reference,references)
        {
            if(reference->valuestring){
               wm_strcat(&ref,reference->valuestring,',');
            }
        }
        cJSON_AddStringToObject(json_summary, "references", ref ? ref : NULL );
        os_free(ref);
    }

    cJSON_AddNumberToObject(json_summary, "passed", passed);
    cJSON_AddNumberToObject(json_summary, "failed", failed);
    cJSON_AddNumberToObject(json_summary, "invalid", invalid);

    float passedf = passed;
    float failedf = failed;
    float score = ((passedf/(failedf+passedf))) * 100;

    if (passed == 0 && failed == 0) {
        score = 0;
    }

    cJSON_AddNumberToObject(json_summary, "total_checks", checks_number);
    cJSON_AddNumberToObject(json_summary, "score", score);

    cJSON_AddNumberToObject(json_summary, "start_time", start_time);
    cJSON_AddNumberToObject(json_summary, "end_time", end_time);

    cJSON_AddStringToObject(json_summary, "hash", integrity_hash);
    cJSON_AddStringToObject(json_summary, "hash_file", integrity_hash_file);

    if (first_scan) {
        cJSON_AddNumberToObject(json_summary, "first_scan", first_scan);
    }

    mdebug1("Sending summary event for file: '%s", file->valuestring);

    if (last_summary_json[id]) {
        cJSON_Delete(last_summary_json[id]);
    }

    last_summary_json[id] = cJSON_Duplicate(json_summary,1);
    wm_sca_send_alert(data,json_summary);
    cJSON_Delete(json_summary);

    return 0;
}

static int wm_sca_send_event_check(wm_sca_t * data,cJSON *event) {

    wm_sca_send_alert(data,event);

    return 0;
}

static cJSON *wm_sca_build_event(const cJSON * const profile, const cJSON * const policy, char **p_alert_msg, int id, const char * const result, const char * const reason) {
    cJSON *json_alert = cJSON_CreateObject();
    cJSON_AddStringToObject(json_alert, "type", "check");
    cJSON_AddNumberToObject(json_alert, "id", id);

    cJSON *name = cJSON_GetObjectItem(policy,"name");
    cJSON *policy_id = cJSON_GetObjectItem(policy,"id");
    cJSON_AddStringToObject(json_alert, "policy", name->valuestring);

    cJSON *check = cJSON_CreateObject();
    cJSON *pm_id = cJSON_GetObjectItem(profile, "id");
    cJSON *title = cJSON_GetObjectItem(profile, "title");
    cJSON *description = cJSON_GetObjectItem(profile, "description");
    cJSON *rationale = cJSON_GetObjectItem(profile, "rationale");
    cJSON *remediation = cJSON_GetObjectItem(profile, "remediation");
    cJSON *rules = cJSON_GetObjectItem(profile, "rules");

    if(!pm_id) {
        mdebug1("No 'id' field found on check.");
        goto error;
    }

    if(!pm_id->valueint) {
        mdebug1("Field 'id' must be a number.");
        goto error;
    }

    cJSON_AddNumberToObject(check, "id", pm_id->valueint);

    if(title){
        if(!title->valuestring) {
            mdebug1("Field 'title' must be a string.");
            goto error;
        }
        cJSON_AddStringToObject(check, "title", title->valuestring);
    } else {
        mdebug1("No 'title' field found on check '%d'.",pm_id->valueint);
        goto error;
    }

    if(!policy_id){
        mdebug1("No 'id' field found on policy.");
        goto error;
    }

    if(description){
        if(!description->valuestring) {
            mdebug1("Field 'description' must be a string.");
            goto error;
        }
        cJSON_AddStringToObject(check, "description", description->valuestring);
    }

    if(rationale){
        if(!rationale->valuestring) {
            mdebug1("Field 'rationale' must be a string.");
            goto error;
        }
        cJSON_AddStringToObject(check, "rationale", rationale->valuestring);
    }

    if(remediation){
        if(!remediation->valuestring) {
            mdebug1("Field 'remediation' must be a string.");
            goto error;
        }
        cJSON_AddStringToObject(check, "remediation", remediation->valuestring);
    }

    cJSON *compliances = cJSON_GetObjectItem(profile, "compliance");

    if(compliances) {
        cJSON *add_compliances = cJSON_CreateObject();
        cJSON *compliance;

        cJSON_ArrayForEach(compliance,compliances)
        {
            if(compliance->child->valuestring){
                cJSON_AddStringToObject(add_compliances,compliance->child->string,compliance->child->valuestring);
            } else if(compliance->child->valuedouble) {
                char double_value[128] = {0};
                snprintf(double_value,128,"%g",compliance->child->valuedouble);

                cJSON_AddStringToObject(add_compliances,compliance->child->string,double_value);
            } else if(compliance->child->valueint) {
                cJSON_AddNumberToObject(add_compliances,compliance->child->string,compliance->child->valueint);
            }
        }

        cJSON_AddItemToObject(check,"compliance",add_compliances);
    }

    cJSON_AddItemToObject(check,"rules", cJSON_Duplicate(rules,1));

    cJSON *references = cJSON_GetObjectItem(profile, "references");

    if(references) {
        cJSON *reference;
        char *ref = NULL;

        cJSON_ArrayForEach(reference,references)
        {
            if(reference->valuestring){
               wm_strcat(&ref,reference->valuestring,',');
            }
        }
        cJSON_AddStringToObject(check, "references", ref ? ref : NULL );
        os_free(ref);
    }

    // Get File or Process from alert
    int i = 0;
    char * final_str_file = NULL;
    char * final_str_directory = NULL;
    char * final_str_process = NULL;
    char * final_str_registry = NULL;
    char * final_str_command = NULL;
    while(i < 255) {

        if(p_alert_msg[i]) {
            char *alert_file = strstr(p_alert_msg[i],"File:");
            char *alert_directory = strstr(p_alert_msg[i],"Directory:");

            if(alert_file){
                alert_file+= 5;
                *alert_file = '\0';
                alert_file++;
                wm_strcat(&final_str_file,alert_file,',');
            } else if (alert_directory){
                alert_directory+= 10;
                *alert_directory = '\0';
                alert_directory++;
                wm_strcat(&final_str_directory,alert_directory,',');
            } else {
                char *alert_process = strstr(p_alert_msg[i],"Process:");
                if(alert_process){
                    alert_process+= 8;
                    *alert_process = '\0';
                    alert_process++;
                    wm_strcat(&final_str_process,alert_process,',');
                } else {
                    char *alert_registry = strstr(p_alert_msg[i],"Registry:");
                    if(alert_registry){
                        alert_registry+= 9;
                        *alert_registry = '\0';
                        alert_registry++;
                        wm_strcat(&final_str_registry,alert_registry,',');
                    } else {
                        char *alert_command = strstr(p_alert_msg[i],"Command:");
                        if(alert_command) {
                            alert_command+= 8;
                            *alert_command = '\0';
                            alert_command++;
                            wm_strcat(&final_str_command,alert_command,',');
                        }
                    }
                }
            }
        } else {
            break;
        }
        i++;
    }

    if(final_str_file) {
        cJSON_AddStringToObject(check, "file", final_str_file);
        os_free(final_str_file);
    }

    if(final_str_directory) {
        cJSON_AddStringToObject(check, "directory", final_str_directory);
        os_free(final_str_directory);
    }

    if(final_str_process) {
       cJSON_AddStringToObject(check, "process", final_str_process);
       os_free(final_str_process);
    }

    if(final_str_registry) {
       cJSON_AddStringToObject(check, "registry", final_str_registry);
       os_free(final_str_registry);
    }

    if(final_str_command) {
       cJSON_AddStringToObject(check, "command", final_str_command);
       os_free(final_str_command);
    }

    if (!strcmp(result, "")) {
        cJSON_AddStringToObject(check, "status", "Not applicable");
        if (reason) {
            cJSON_AddStringToObject(check, "reason", reason);
        }
    } else {
        cJSON_AddStringToObject(check, "result", result);
    }

    if(!policy_id->valuestring) {
        mdebug1("Field 'id' must be a string");
        goto error;
    }

    cJSON_AddStringToObject(json_alert, "policy_id", policy_id->valuestring);
    cJSON_AddItemToObject(json_alert,"check",check);

    return json_alert;

error:

    if(json_alert){
        cJSON_Delete(json_alert);
    }

    return NULL;
}

static int wm_sca_check_hash(OSHash * const cis_db_hash, const char * const result,
    const cJSON * const profile, const cJSON * const event, int check_index,int policy_index)
{
    cis_db_info_t *hashed_result = NULL;
    char id_hashed[OS_SIZE_128];
    int ret_add = 0;
    cJSON *pm_id = cJSON_GetObjectItem(profile, "id");
    int alert = 1;

    if(!pm_id) {
        return 0;
    }

    if(!pm_id->valueint) {
        return 0;
    }

    sprintf(id_hashed, "%d", pm_id->valueint);

    hashed_result = OSHash_Get(cis_db_hash, id_hashed);

    cis_db_info_t *elem;

    os_calloc(1, sizeof(cis_db_info_t), elem);
    if (!result) {
	    os_strdup("",elem->result);
	} else {
	    os_strdup(result,elem->result);
	}

    cJSON *obj = cJSON_Duplicate(event,1);
    elem->event = NULL;

    if(obj) {
        elem->event = obj;

        if (!hashed_result) {
            if (ret_add = OSHash_Add(cis_db_hash,id_hashed,elem), ret_add != 2) {
                merror("Unable to update hash table for check: %d", pm_id->valueint);
                os_free(elem->result);
                cJSON_Delete(elem->event);
                os_free(elem);
                return 0;
            }
        } else {
            if(strcmp(elem->result,hashed_result->result) == 0) {
                alert = 0;
            }

            if (ret_add = OSHash_Update(cis_db_hash,id_hashed,elem), ret_add != 1) {
                merror("Unable to update hash table for check: %d", pm_id->valueint);
                os_free(elem->result);
                cJSON_Delete(elem->event);
                os_free(elem);
                return 0;
            }
        }

        cis_db_for_hash[policy_index].elem[check_index] = elem;
        return alert;

    }

    os_free(elem->result);
    os_free(elem);
    return 0;

}

static void wm_sca_free_hash_data(cis_db_info_t *event) {

    if(event) {
        if(event->result){
            os_free(event->result);
        }

        if(event->event) {
            cJSON_Delete(event->event);
        }
        os_free(event);
    }
}

static char *wm_sca_hash_integrity(int policy_index) {
    char *str = NULL;

    int i;
    for(i = 0; cis_db_for_hash[policy_index].elem[i]; i++) {
        cis_db_info_t *event;
        event = cis_db_for_hash[policy_index].elem[i];

        if(event->result){
            wm_strcat(&str,event->result,':');
        }
    }

    if(str) {
        os_sha256 hash;
        OS_SHA256_String(str, hash);
        os_free(str);
        return strdup(hash);
    }

    return NULL;
}

static char *wm_sca_hash_integrity_file(const char *file) {

    char *hash_file = NULL;
    os_malloc(65*sizeof(char), hash_file);

    if(OS_SHA256_File(file, hash_file, OS_TEXT) != 0){
        merror("Unable to calculate SHA256 for file '%s'", file);
        os_free(hash_file);
        return NULL;
    }

    return hash_file;
}

static void *wm_sca_dump_db_thread(wm_sca_t * data) {
    int i;

    while(1) {
        request_dump_t *request;

        if (request = queue_pop_ex(request_queue), request) {

#ifndef WIN32
            int random = os_random();
            if (random < 0)
                random = -random;
#else
            unsigned int random1 = os_random();
            unsigned int random2 = os_random();

            char random_id[OS_MAXSTR];
            snprintf(random_id, OS_MAXSTR - 1, "%u%u", random1, random2);

            int random = atoi(random_id);
            if (random < 0)
                random = -random;
#endif
            random = random % data->request_db_interval;

            if(random == 0) {
                random += 5;
            }

            unsigned int time = random;

            if (request->first_scan) {
                wm_delay(2000);
                mdebug1("Sending first scan results for policy '%s'.", data->profile[request->policy_index]->profile);
            } else {
                minfo("Integration checksum failed for policy '%s'. Resending scan results in %d seconds.", data->profile[request->policy_index]->profile,random);
                wm_delay(1000 * time);
                mdebug1("Dumping results to SCA DB for policy index '%u'",request->policy_index);
            }

            int scan_id = -1;
            w_rwlock_wrlock(&dump_rwlock);

            for(i = 0; cis_db_for_hash[request->policy_index].elem[i]; i++) {
                cis_db_info_t *event;
                event = cis_db_for_hash[request->policy_index].elem[i];

                if (event) {
                    if(event->event){
                        cJSON *db_obj;
                        db_obj = event->event;

                        if(scan_id == -1) {
                            cJSON * scan_id_obj = cJSON_GetObjectItem(db_obj, "id");

                            if(scan_id_obj) {
                                scan_id =  scan_id_obj->valueint;
                            }
                        }
                        wm_sca_send_event_check(data,db_obj);
                    }
                }
            }

            wm_delay(5000);

            int elements_sent = i;
            mdebug1("Sending end of dump control event");

            wm_sca_send_dump_end(data,elements_sent,data->profile[request->policy_index]->policy_id,scan_id);

            wm_delay(2000);

            /* Send summary only for first scan */
            if (request->first_scan) {
                /* Send summary */
                cJSON_DeleteItemFromObject(last_summary_json[request->policy_index],"first_scan");
                /* Force alert */
                cJSON_AddStringToObject(last_summary_json[request->policy_index], "force_alert", "1");

                wm_sca_send_alert(data,last_summary_json[request->policy_index]);
            }

            mdebug1("Finished dumping scan results to SCA DB for policy index '%u'",request->policy_index);

            w_rwlock_unlock(&dump_rwlock);
            os_free(request);
        }
    }

    return NULL;
}


static int wm_sca_send_dump_end(wm_sca_t * data, unsigned int elements_sent,char * policy_id, int scan_id) {
    cJSON *dump_event = cJSON_CreateObject();

    cJSON_AddStringToObject(dump_event, "type", "dump_end");
    cJSON_AddStringToObject(dump_event, "policy_id", policy_id);
    cJSON_AddNumberToObject(dump_event, "elements_sent", elements_sent);
    cJSON_AddNumberToObject(dump_event, "scan_id", scan_id);

    wm_sca_send_alert(data,dump_event);

    cJSON_Delete(dump_event);

    return 0;
}

#ifdef WIN32
void wm_sca_push_request_win(char * msg){
    char *db = strchr(msg,':');

    if(!strncmp(msg,WM_CONFIGURATION_ASSESSMENT_DB_DUMP,strlen(WM_CONFIGURATION_ASSESSMENT_DB_DUMP)) && db) {

        *db++ = '\0';

        /* Check for first scan */
        char *first_scan = strchr(db,':');

        if (!first_scan) {
            mdebug1("First scan flag missing");
            return;
        }

        *first_scan++ = '\0';

        /* Search DB */
        int i;

        if(data_win) {
            for(i = 0; data_win->profile[i]; i++) {
                if(!data_win->profile[i]->enabled){
                    continue;
                }

                if(data_win->profile[i]->policy_id) {
                    char *endl;

                    endl = strchr(db,'\n');

                    if(endl){
                        *endl = '\0';
                    }

                    if(strcmp(data_win->profile[i]->policy_id,db) == 0){
                        request_dump_t *request;
                        os_calloc(1, sizeof(request_dump_t),request);

                        request->policy_index = i;
                        request->first_scan = atoi(first_scan);

                        if(queue_push_ex(request_queue,request) < 0) {
                            os_free(request);
                            mdebug1("Could not push policy index to queue");
                        }
                        break;
                    }
                }
            }
        }
    }
}

#endif

#ifndef WIN32
static void * wm_sca_request_thread(wm_sca_t * data) {

    /* Create request socket */
    int cfga_queue;
    if ((cfga_queue = StartMQ(CFGASSESSMENTQUEUEPATH, READ)) < 0) {
        merror(QUEUE_ERROR, CFGASSESSMENTQUEUEPATH, strerror(errno));
        pthread_exit(NULL);
    }

    int recv = 0;
    char *buffer = NULL;
    os_calloc(OS_MAXSTR + 1,sizeof(char),buffer);

    while (1) {
        if (recv = OS_RecvUnix(cfga_queue, OS_MAXSTR, buffer),recv) {
            buffer[recv] = '\0';

            char *db = strchr(buffer,':');

            if(!strncmp(buffer,WM_CONFIGURATION_ASSESSMENT_DB_DUMP,strlen(WM_CONFIGURATION_ASSESSMENT_DB_DUMP)) && db) {

                *db++ = '\0';

                /* Check for first scan */
                char *first_scan = strchr(db,':');

                if (!first_scan) {
                    mdebug1("First scan flag missing");
                    continue;
                }

                *first_scan++ = '\0';

                /* Search DB */
                int i;
                for(i = 0; data->profile[i]; i++) {
                    if(!data->profile[i]->enabled){
                        continue;
                    }

                    if(data->profile[i]->policy_id) {
                        char *endl;

                        endl = strchr(db,'\n');

                        if(endl){
                            *endl = '\0';
                        }

                        if(strcmp(data->profile[i]->policy_id,db) == 0){
                            request_dump_t *request;
                            os_calloc(1, sizeof(request_dump_t),request);

                            request->policy_index = i;
                            request->first_scan = atoi(first_scan);

                            if(queue_push_ex(request_queue,request) < 0) {
                                os_free(request);
                                mdebug1("Could not push policy index to queue");
                            }
                            break;
                        }
                    }
                }
            }
        }
    }

    return NULL;
}
#endif
static void wm_sca_summary_increment_passed() {
    summary_passed++;
}

static void wm_sca_summary_increment_failed() {
    summary_failed++;
}

static void wm_sca_summary_increment_invalid() {
    summary_invalid++;
}

static void wm_sca_reset_summary() {
    summary_failed = 0;
    summary_passed = 0;
    summary_invalid = 0;
}

cJSON *wm_sca_dump(const wm_sca_t *data) {
    cJSON *root = cJSON_CreateObject();
    cJSON *wm_wd = cJSON_CreateObject();

    cJSON_AddStringToObject(wm_wd, "enabled", data->enabled ? "yes" : "no");
    cJSON_AddStringToObject(wm_wd, "scan_on_start", data->scan_on_start ? "yes" : "no");
    cJSON_AddStringToObject(wm_wd, "skip_nfs", data->skip_nfs ? "yes" : "no");
    if (data->interval) cJSON_AddNumberToObject(wm_wd, "interval", data->interval);
    if (data->scan_day) cJSON_AddNumberToObject(wm_wd, "day", data->scan_day);

    switch (data->scan_wday) {
        case 0:
            cJSON_AddStringToObject(wm_wd, "wday", "sunday");
            break;
        case 1:
            cJSON_AddStringToObject(wm_wd, "wday", "monday");
            break;
        case 2:
            cJSON_AddStringToObject(wm_wd, "wday", "tuesday");
            break;
        case 3:
            cJSON_AddStringToObject(wm_wd, "wday", "wednesday");
            break;
        case 4:
            cJSON_AddStringToObject(wm_wd, "wday", "thursday");
            break;
        case 5:
            cJSON_AddStringToObject(wm_wd, "wday", "friday");
            break;
        case 6:
            cJSON_AddStringToObject(wm_wd, "wday", "saturday");
            break;
        default:
            break;
    }
    if (data->scan_time) cJSON_AddStringToObject(wm_wd, "time", data->scan_time);

    if (data->profile && *data->profile) {
        cJSON *profiles = cJSON_CreateArray();
        int i;
        for (i=0;data->profile[i];i++) {
            if(data->profile[i]->enabled == 1){
                cJSON_AddStringToObject(profiles,"policy",data->profile[i]->profile);
            }
        }
        cJSON_AddItemToObject(wm_wd,"policies",profiles);
    }

    cJSON_AddItemToObject(root,"sca",wm_wd);


    return root;
}

static int append_msg_to_vm_scat (wm_sca_t * const data, const char * const msg)
{
    /* Already present */
    if (w_is_str_in_array(data->alert_msg, msg)) {
        return 1;
    }

    int i = 0;
    while (data->alert_msg[i] && (i < 255)) {
        i++;
    }

    if (!data->alert_msg[i]) {
        os_strdup(msg, data->alert_msg[i]);
    }
    return 0;
}
