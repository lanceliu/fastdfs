/**
* Copyright (C) 2008 Happy Fish / YuQing
*
* FastDFS may be copied only under the terms of the GNU General
* Public License V3, which may be found in the FastDFS source kit.
* Please visit the FastDFS Home Page http://www.csource.org/ for more detail.
**/

//storage_service.c

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include "fdfs_define.h"
#include "logger.h"
#include "fdfs_global.h"
#include "sockopt.h"
#include "shared_func.h"
#include "pthread_func.h"
#include "tracker_types.h"
#include "tracker_proto.h"
#include "storage_service.h"
#include "storage_func.h"
#include "storage_sync.h"
#include "storage_global.h"
#include "base64.h"
#include "hash.h"
#include "fdht_client.h"
#include "fdfs_global.h"
#include "tracker_client.h"
#include "storage_client.h"
#include "storage_nio.h"
#include "storage_dio.h"
#include "storage_sync.h"
#include "trunk_mem.h"
#include "trunk_sync.h"
#include "trunk_client.h"

pthread_mutex_t g_storage_thread_lock;
int g_storage_thread_count = 0;
static int extra_open_flags = 0;
static int last_stat_change_count = 1;  //for sync to stat file
static int64_t temp_file_sequence = 0;

static pthread_mutex_t path_index_thread_lock;
static pthread_mutex_t stat_count_thread_lock;

static void *work_thread_entrance(void* arg);

extern int storage_client_create_link(TrackerServerInfo *pTrackerServer, \
		TrackerServerInfo *pStorageServer, const char *master_filename,\
		const char *src_filename, const int src_filename_len, \
		const char *src_file_sig, const int src_file_sig_len, \
		const char *group_name, const char *prefix_name, \
		const char *file_ext_name, \
		char *remote_filename, int *filename_len);

static int storage_do_delete_file(struct fast_task_info *pTask, \
		DeleteFileLogCallback log_callback, \
		FileDealDoneCallback done_callback, \
		const int store_path_index);

static int storage_write_to_file(struct fast_task_info *pTask, \
		const int64_t file_offset, const int64_t upload_bytes, \
		const int buff_offset, TaskDealFunc deal_func, \
		FileDealDoneCallback done_callback, \
		DisconnectCleanFunc clean_func, const int store_path_index);

static int storage_read_from_file(struct fast_task_info *pTask, \
		const int64_t file_offset, const int64_t download_bytes, \
		FileDealDoneCallback done_callback, \
		const int store_path_index);

static int storage_service_upload_file_done(struct fast_task_info *pTask);

#define STORAGE_STATUE_DEAL_FILE	 123456   //status for read or write file

#define FDHT_KEY_NAME_FILE_ID	"fid"
#define FDHT_KEY_NAME_REF_COUNT	"ref"
#define FDHT_KEY_NAME_FILE_SIG	"sig"

#define FILE_SIGNATURE_SIZE	24

#define STORAGE_GEN_FILE_SIGNATURE(file_size, hash_codes, sig_buff) \
	long2buff(file_size, sig_buff); \
	int2buff(hash_codes[0], sig_buff+8);  \
	int2buff(hash_codes[1], sig_buff+12); \
	int2buff(hash_codes[2], sig_buff+16); \
	int2buff(hash_codes[3], sig_buff+20); \


typedef struct 
{
	char src_true_filename[128];
	char src_file_sig[64];
	int src_file_sig_len;
} SourceFileInfo;

static int storage_create_link_core(struct fast_task_info *pTask, \
		const int store_path_index, SourceFileInfo *pSourceFileInfo, \
		const char *src_filename, const char *master_filename, \
		const int master_filename_len, \
		const char *prefix_name, const char *file_ext_name, \
		char *filename, int *filename_len);

static FDFSStorageServer *get_storage_server(const char *ip_addr)
{
	FDFSStorageServer targetServer;
	FDFSStorageServer *pTargetServer;
	FDFSStorageServer **ppFound;

	memset(&targetServer, 0, sizeof(targetServer));
	strcpy(targetServer.server.ip_addr, ip_addr);

	pTargetServer = &targetServer;
	ppFound = (FDFSStorageServer **)bsearch(&pTargetServer, \
		g_sorted_storages, g_storage_count, \
		sizeof(FDFSStorageServer *), storage_cmp_by_ip_addr);
	if (ppFound == NULL)
	{
		return NULL;
	}
	else
	{
		return *ppFound;
	}
}

#define CHECK_AND_WRITE_TO_STAT_FILE1  \
		pthread_mutex_lock(&stat_count_thread_lock); \
\
		if (pClientInfo->pSrcStorage == NULL) \
		{ \
			pClientInfo->pSrcStorage = get_storage_server( \
					pClientInfo->tracker_client_ip); \
		} \
		if (pClientInfo->pSrcStorage != NULL) \
		{ \
			pClientInfo->pSrcStorage->last_sync_src_timestamp = \
					pFileContext->timestamp2log; \
			g_sync_change_count++; \
		} \
\
		g_storage_stat.last_sync_update = time(NULL); \
		++g_stat_change_count; \
		pthread_mutex_unlock(&stat_count_thread_lock);

#define CHECK_AND_WRITE_TO_STAT_FILE1_WITH_BYTES( \
		total_bytes, success_bytes, bytes)  \
		pthread_mutex_lock(&stat_count_thread_lock); \
\
		if (pClientInfo->pSrcStorage == NULL) \
		{ \
			pClientInfo->pSrcStorage = get_storage_server( \
					pClientInfo->tracker_client_ip); \
		} \
		if (pClientInfo->pSrcStorage != NULL) \
		{ \
			pClientInfo->pSrcStorage->last_sync_src_timestamp = \
					pFileContext->timestamp2log; \
			g_sync_change_count++; \
		} \
\
		g_storage_stat.last_sync_update = time(NULL); \
		total_bytes += bytes; \
		success_bytes += bytes; \
		++g_stat_change_count; \
		pthread_mutex_unlock(&stat_count_thread_lock);

#define CHECK_AND_WRITE_TO_STAT_FILE2(total_count, success_count)  \
		pthread_mutex_lock(&stat_count_thread_lock); \
		total_count++; \
		success_count++; \
		++g_stat_change_count; \
		pthread_mutex_unlock(&stat_count_thread_lock);

#define CHECK_AND_WRITE_TO_STAT_FILE2_WITH_BYTES(total_count, success_count, \
		total_bytes, success_bytes, bytes)  \
		pthread_mutex_lock(&stat_count_thread_lock); \
		total_count++; \
		success_count++; \
		total_bytes += bytes; \
		success_bytes += bytes; \
		++g_stat_change_count; \
		pthread_mutex_unlock(&stat_count_thread_lock);

#define CHECK_AND_WRITE_TO_STAT_FILE3(total_count, success_count, timestamp)  \
		pthread_mutex_lock(&stat_count_thread_lock); \
		total_count++; \
		success_count++; \
		timestamp = time(NULL); \
		++g_stat_change_count;  \
		pthread_mutex_unlock(&stat_count_thread_lock);

#define CHECK_AND_WRITE_TO_STAT_FILE3_WITH_BYTES(total_count, success_count, \
		timestamp, total_bytes, success_bytes, bytes)  \
		pthread_mutex_lock(&stat_count_thread_lock); \
		total_count++; \
		success_count++; \
		timestamp = time(NULL); \
		total_bytes += bytes; \
		success_bytes += bytes; \
		++g_stat_change_count;  \
		pthread_mutex_unlock(&stat_count_thread_lock);

static void storage_delete_file_log_error(struct fast_task_info *pTask, \
			const int err_no)
{
	StorageFileContext *pFileContext;

	pFileContext =  &(((StorageClientInfo *)pTask->arg)->file_context);
	logError("file: "__FILE__", line: %d, " \
		"client ip: %s, delete file %s fail," \
		"errno: %d, error info: %s", __LINE__, \
		pTask->client_ip, pFileContext->filename, \
		err_no, STRERROR(err_no));
}

static void storage_sync_delete_file_log_error(struct fast_task_info *pTask, \
			const int err_no)
{
	StorageFileContext *pFileContext;

	pFileContext =  &(((StorageClientInfo *)pTask->arg)->file_context);
	if (err_no == ENOENT)
	{
		logWarning("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, " \
			"file %s not exist, " \
			"maybe delete later?", __LINE__, \
			STORAGE_PROTO_CMD_SYNC_DELETE_FILE, \
			pTask->client_ip, pFileContext->filename);
	}
	else
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, delete file %s fail," \
			"errno: %d, error info: %s", \
			__LINE__, pTask->client_ip, \
			pFileContext->filename, err_no, STRERROR(err_no));
	}
}

static void storage_sync_delete_file_done_callback(struct fast_task_info *pTask, \
			const int err_no)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	TrackerHeader *pHeader;
	int result;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	if (err_no == 0 && pFileContext->sync_flag != '\0')
	{
		result = storage_binlog_write(pFileContext->timestamp2log, \
			pFileContext->sync_flag, pFileContext->fname2log);
	}
	else
	{
		result = err_no;
	}

	if (result == 0)
	{
		CHECK_AND_WRITE_TO_STAT_FILE1
	}

	pClientInfo->total_length = sizeof(TrackerHeader);
	pClientInfo->total_offset = 0;
	pTask->length = pClientInfo->total_length;
	pHeader = (TrackerHeader *)pTask->data;
	pHeader->status = result;
	pHeader->cmd = STORAGE_PROTO_CMD_RESP;
	long2buff(pClientInfo->total_length - sizeof(TrackerHeader), \
			pHeader->pkg_len);

	storage_nio_notify(pTask);
}


static int storage_sync_copy_file_rename_filename( \
		StorageFileContext *pFileContext)
{
	char full_filename[MAX_PATH_SIZE + 256];
	char true_filename[128];
	int filename_len;
	int result;
	int store_path_index;

	filename_len = strlen(pFileContext->fname2log);
	if ((result=storage_split_filename_ex(pFileContext->fname2log, \
		&filename_len, true_filename, &store_path_index)) != 0)
	{
		return result;
	}

	snprintf(full_filename, sizeof(full_filename), \
			"%s/data/%s", g_store_paths[store_path_index], \
			true_filename);
	if (rename(pFileContext->filename, full_filename) != 0)
	{
		result = errno != 0 ? errno : EPERM;
		logWarning("file: "__FILE__", line: %d, " \
			"rename %s to %s fail, " \
			"errno: %d, error info: %s", __LINE__, \
			pFileContext->filename, full_filename, \
			result, STRERROR(result));
		return result;
	}

	return 0;
}

static void storage_sync_copy_file_done_callback(struct fast_task_info *pTask, \
			const int err_no)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	TrackerHeader *pHeader;
	int result;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);
	result = err_no;
	if (result == 0)
	{
		if (pFileContext->op == FDFS_STORAGE_FILE_OP_WRITE) 
		{
			if (!pFileContext->extra_info.upload.if_trunk_file)
			{
			result = storage_sync_copy_file_rename_filename( \
					pFileContext);
			}

			if (result == 0)
			{
			storage_binlog_write(pFileContext->timestamp2log, \
			pFileContext->sync_flag, pFileContext->fname2log);
			}
		}
	}

	if (pFileContext->op == FDFS_STORAGE_FILE_OP_WRITE)
	{
		if (result == 0)
		{
			CHECK_AND_WRITE_TO_STAT_FILE1_WITH_BYTES( \
				g_storage_stat.total_sync_in_bytes, \
				g_storage_stat.success_sync_in_bytes, \
				pFileContext->end - pFileContext->start)
		}
	}
	else  //FDFS_STORAGE_FILE_OP_DISCARD
	{
		if (result == 0)
		{
			CHECK_AND_WRITE_TO_STAT_FILE1
		}

		result = EEXIST;
	}
	if (result != 0)
	{
		pthread_mutex_lock(&stat_count_thread_lock);
		g_storage_stat.total_sync_in_bytes += \
				pClientInfo->total_offset;
		pthread_mutex_unlock(&stat_count_thread_lock);
	}

	pClientInfo->total_length = sizeof(TrackerHeader);
	pClientInfo->total_offset = 0;
	pTask->length = pClientInfo->total_length;
	pHeader = (TrackerHeader *)pTask->data;
	pHeader->status = result;
	pHeader->cmd = STORAGE_PROTO_CMD_RESP;
	long2buff(pClientInfo->total_length - sizeof(TrackerHeader), \
			pHeader->pkg_len);

	storage_nio_notify(pTask);
}

static void storage_sync_append_file_done_callback( \
		struct fast_task_info *pTask, const int err_no)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	TrackerHeader *pHeader;
	int result;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);
	result = err_no;

	if (pFileContext->op == FDFS_STORAGE_FILE_OP_APPEND)
	{
		if (result == 0)
		{
		storage_binlog_write(pFileContext->timestamp2log, \
			pFileContext->sync_flag, pFileContext->fname2log);

			CHECK_AND_WRITE_TO_STAT_FILE1_WITH_BYTES( \
				g_storage_stat.total_sync_in_bytes, \
				g_storage_stat.success_sync_in_bytes, \
				pFileContext->end - pFileContext->start)
		}
	}
	else  //FDFS_STORAGE_FILE_OP_DISCARD
	{
		if (result == 0)
		{
			struct stat file_stat;

			if (lstat(pFileContext->filename, &file_stat) != 0)
			{
				result = errno != 0 ? errno : ENOENT;
				if (result != ENOENT)
				{
				logError("file: "__FILE__", line: %d, " \
					"client ip:%s, call stat file %s fail"\
					", errno: %d, error info: %s", \
					__LINE__, pTask->client_ip, \
					pFileContext->filename, 
					result, STRERROR(result));
				}
			}
			else if (!S_ISREG(file_stat.st_mode))
			{
				result = EEXIST;
			}
			else if (file_stat.st_size < pFileContext->end)
			{
				result = ENOENT;  //need to resync
			}
			else
			{
				result = EEXIST;
			}

			CHECK_AND_WRITE_TO_STAT_FILE1
		}
	}

	if (result != 0)
	{
		pthread_mutex_lock(&stat_count_thread_lock);
		g_storage_stat.total_sync_in_bytes += \
				pClientInfo->total_offset;
		pthread_mutex_unlock(&stat_count_thread_lock);
	}

	pClientInfo->total_length = sizeof(TrackerHeader);
	pClientInfo->total_offset = 0;
	pTask->length = pClientInfo->total_length;
	pHeader = (TrackerHeader *)pTask->data;
	pHeader->status = result;
	pHeader->cmd = STORAGE_PROTO_CMD_RESP;
	long2buff(pClientInfo->total_length - sizeof(TrackerHeader), \
			pHeader->pkg_len);

	storage_nio_notify(pTask);
}

static void storage_get_metadata_done_callback(struct fast_task_info *pTask, \
			const int err_no)
{
	TrackerHeader *pHeader;

	if (err_no != 0)
	{
		pthread_mutex_lock(&stat_count_thread_lock);
		g_storage_stat.total_get_meta_count++;
		pthread_mutex_unlock(&stat_count_thread_lock);

		if (pTask->length == sizeof(TrackerHeader)) //never response
		{
			pHeader = (TrackerHeader *)pTask->data;
			pHeader->status = err_no;
			storage_nio_notify(pTask);
		}
		else
		{
			task_finish_clean_up(pTask);
		}
	}
	else
	{
		CHECK_AND_WRITE_TO_STAT_FILE2( \
			g_storage_stat.total_get_meta_count, \
			g_storage_stat.success_get_meta_count)

		storage_nio_notify(pTask);
	}
}

static void storage_download_file_done_callback(struct fast_task_info *pTask, \
			const int err_no)
{
	StorageFileContext *pFileContext;
	TrackerHeader *pHeader;

	pFileContext = &(((StorageClientInfo *)pTask->arg)->file_context);
	if (err_no != 0)
	{
		pthread_mutex_lock(&stat_count_thread_lock);
		g_storage_stat.total_download_count++;
		g_storage_stat.total_download_bytes += \
				pFileContext->offset - pFileContext->start;
		pthread_mutex_unlock(&stat_count_thread_lock);

		if (pTask->length == sizeof(TrackerHeader)) //never response
		{
			pHeader = (TrackerHeader *)pTask->data;
			pHeader->status = err_no;
			storage_nio_notify(pTask);
		}
		else
		{
			task_finish_clean_up(pTask);
		}
	}
	else
	{
		CHECK_AND_WRITE_TO_STAT_FILE2_WITH_BYTES( \
			g_storage_stat.total_download_count, \
			g_storage_stat.success_download_count, \
			g_storage_stat.total_download_bytes, \
			g_storage_stat.success_download_bytes, \
			pFileContext->end - pFileContext->start)

		storage_nio_notify(pTask);
	}
}

static int storage_do_delete_meta_file(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	GroupArray *pGroupArray;
	char meta_filename[MAX_PATH_SIZE + 256];
	char true_filename[128];
	char full_filename[MAX_PATH_SIZE + 256];
	char value[128];
	FDHTKeyInfo key_info_fid;
	FDHTKeyInfo key_info_ref;
	FDHTKeyInfo key_info_sig;
	char *pValue;
	int value_len;
	int src_file_nlink;
	int result;
	int store_path_index;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	sprintf(meta_filename, "%s"STORAGE_META_FILE_EXT, \
			pFileContext->filename);
	if (fileExists(meta_filename))
	{
		if (unlink(meta_filename) != 0)
		{
			if (errno != ENOENT)
			{
				result = errno != 0 ? errno : EACCES;
				logError("file: "__FILE__", line: %d, " \
					"client ip: %s, delete file %s fail," \
					"errno: %d, error info: %s", __LINE__,\
					pTask->client_ip, meta_filename, \
					result, STRERROR(result));
				return result;
			}
		}
		else
		{
			sprintf(meta_filename, "%s"STORAGE_META_FILE_EXT, \
					pFileContext->fname2log);
			result = storage_binlog_write(time(NULL), \
				STORAGE_OP_TYPE_SOURCE_DELETE_FILE, meta_filename);
			if (result != 0)
			{
				return result;
			}
		}
	}

	src_file_nlink = -1;
	if (g_check_file_duplicate)
	{
		pGroupArray=&((g_nio_thread_data+pClientInfo->nio_thread_index)\
				->group_array);
		memset(&key_info_sig, 0, sizeof(key_info_sig));
		key_info_sig.namespace_len = g_namespace_len;
		memcpy(key_info_sig.szNameSpace, g_key_namespace, \
				g_namespace_len);
		key_info_sig.obj_id_len = snprintf(\
				key_info_sig.szObjectId, \
				sizeof(key_info_sig.szObjectId), "%s/%s", \
				g_group_name, pFileContext->fname2log);

		key_info_sig.key_len = sizeof(FDHT_KEY_NAME_FILE_SIG)-1;
		memcpy(key_info_sig.szKey, FDHT_KEY_NAME_FILE_SIG, \
				key_info_sig.key_len);
		pValue = value;
		value_len = sizeof(value) - 1;
		result = fdht_get_ex1(pGroupArray, g_keep_alive, \
				&key_info_sig, FDHT_EXPIRES_NONE, \
				&pValue, &value_len, malloc);
		if (result == 0)
		{
			memcpy(&key_info_fid, &key_info_sig, \
					sizeof(FDHTKeyInfo));
			key_info_fid.obj_id_len = value_len;
			memcpy(key_info_fid.szObjectId, pValue, \
					value_len);

			key_info_fid.key_len = sizeof(FDHT_KEY_NAME_FILE_ID) - 1;
			memcpy(key_info_fid.szKey, FDHT_KEY_NAME_FILE_ID, \
					key_info_fid.key_len);
			value_len = sizeof(value) - 1;
			result = fdht_get_ex1(pGroupArray, \
					g_keep_alive, &key_info_fid, \
					FDHT_EXPIRES_NONE, &pValue, \
					&value_len, malloc);
			if (result == 0)
			{
				memcpy(&key_info_ref, &key_info_sig, \
						sizeof(FDHTKeyInfo));
				key_info_ref.obj_id_len = value_len;
				memcpy(key_info_ref.szObjectId, pValue, 
						value_len);
				key_info_ref.key_len = \
					sizeof(FDHT_KEY_NAME_REF_COUNT)-1;
				memcpy(key_info_ref.szKey, \
					FDHT_KEY_NAME_REF_COUNT, \
					key_info_ref.key_len);
				value_len = sizeof(value) - 1;

				result = fdht_get_ex1(pGroupArray, \
						g_keep_alive, &key_info_ref, \
						FDHT_EXPIRES_NONE, &pValue, \
						&value_len, malloc);
				if (result == 0)
				{
					*(pValue + value_len) = '\0';
					src_file_nlink = atoi(pValue);
				}
				else if (result != ENOENT)
				{
					logError("file: "__FILE__", line: %d, " \
						"client ip: %s, fdht_get fail," \
						"errno: %d, error info: %s", \
						__LINE__, pTask->client_ip, \
						result, STRERROR(result));
					return result;
				}
			}
			else if (result != ENOENT)
			{
				logError("file: "__FILE__", line: %d, " \
					"client ip: %s, fdht_get fail," \
					"errno: %d, error info: %s", \
					__LINE__, pTask->client_ip, \
					result, STRERROR(result));
				return result;
			}
		}
		else if (result != ENOENT)
		{
			logError("file: "__FILE__", line: %d, " \
					"client ip: %s, fdht_get fail," \
					"errno: %d, error info: %s", \
					__LINE__, pTask->client_ip, \
					result, STRERROR(result));
			return result;
		}
	}

	if (src_file_nlink < 0)
	{
		return 0;
	}

	if (g_check_file_duplicate)
	{
		char *pSeperator;

		pGroupArray=&((g_nio_thread_data+pClientInfo->nio_thread_index)\
				->group_array);
		if ((result=fdht_delete_ex(pGroupArray, g_keep_alive, \
						&key_info_sig)) != 0)
		{
			logWarning("file: "__FILE__", line: %d, " \
				"client ip: %s, fdht_delete fail," \
				"errno: %d, error info: %s", \
				__LINE__, pTask->client_ip, \
				result, STRERROR(result));
		}

		value_len = sizeof(value) - 1;
		result = fdht_inc_ex(pGroupArray, g_keep_alive, \
				&key_info_ref, FDHT_EXPIRES_NEVER, -1, \
				value, &value_len);
		if (result != 0)
		{
			logWarning("file: "__FILE__", line: %d, " \
				"client ip: %s, fdht_inc fail," \
				"errno: %d, error info: %s", \
				__LINE__, pTask->client_ip, \
				result, STRERROR(result));
			return result;
		}

		if (!(value_len == 1 && *value == '0')) //value == 0
		{
			return 0;
		}

		if ((result=fdht_delete_ex(pGroupArray, g_keep_alive, \
						&key_info_fid)) != 0)
		{
			logWarning("file: "__FILE__", line: %d, " \
				"client ip: %s, fdht_delete fail," \
				"errno: %d, error info: %s", \
				__LINE__, pTask->client_ip, \
				result, STRERROR(result));
		}
		if ((result=fdht_delete_ex(pGroupArray, g_keep_alive, \
						&key_info_ref)) != 0)
		{
			logWarning("file: "__FILE__", line: %d, " \
				"client ip: %s, fdht_delete fail," \
				"errno: %d, error info: %s", \
				__LINE__, pTask->client_ip, \
				result, STRERROR(result));
		}

		*(key_info_ref.szObjectId+key_info_ref.obj_id_len)='\0';
		pSeperator = strchr(key_info_ref.szObjectId, '/');
		if (pSeperator == NULL)
		{
			logWarning("file: "__FILE__", line: %d, " \
				"invalid file_id: %s", __LINE__, \
				key_info_ref.szObjectId);
			return 0;
		}

		pSeperator++;
		value_len = key_info_ref.obj_id_len - (pSeperator - \
				key_info_ref.szObjectId);
		memcpy(value, pSeperator, value_len + 1);
		if ((result=storage_split_filename_ex(value, &value_len, \
				true_filename, &store_path_index)) != 0)
		{
			return result;
		}
		if ((result=fdfs_check_data_filename(true_filename, \
					value_len)) != 0)
		{
			return result;
		}

		sprintf(full_filename, "%s/data/%s", \
				g_store_paths[store_path_index], true_filename);
		if (unlink(full_filename) != 0)
		{
			logWarning("file: "__FILE__", line: %d, " \
				"client ip: %s, delete source file " \
				"%s fail, errno: %d, error info: %s", \
				__LINE__, pTask->client_ip, \
				full_filename, errno, STRERROR(errno));
			return 0;
		}

		storage_binlog_write(time(NULL), \
				STORAGE_OP_TYPE_SOURCE_DELETE_FILE, value);
		pFileContext->delete_flag |= STORAGE_DELETE_FLAG_FILE;
	}

	return 0;
}

static void storage_delete_fdfs_file_done_callback( \
		struct fast_task_info *pTask, const int err_no)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	TrackerHeader *pHeader;
	int result;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	if (err_no == 0)
	{
		if (pFileContext->extra_info.upload.if_trunk_file)
		{
			trunk_client_trunk_free_space( \
				&(pFileContext->extra_info.upload.trunk_info));
		}

		result = storage_binlog_write(time(NULL), \
			STORAGE_OP_TYPE_SOURCE_DELETE_FILE, \
			pFileContext->fname2log);
	}
	else
	{
		result = err_no;
	}

	if (result == 0)
	{
		result = storage_do_delete_meta_file(pTask);
	}

	if (result != 0)
	{
		pthread_mutex_lock(&stat_count_thread_lock);
		if (pFileContext->delete_flag == STORAGE_DELETE_FLAG_NONE ||\
				(pFileContext->delete_flag & STORAGE_DELETE_FLAG_FILE))
		{
			g_storage_stat.total_delete_count++;
		}
		if (pFileContext->delete_flag & STORAGE_DELETE_FLAG_LINK)
		{
			g_storage_stat.total_delete_link_count++;
		}
		pthread_mutex_unlock(&stat_count_thread_lock);
	}
	else
	{
		if (pFileContext->delete_flag & STORAGE_DELETE_FLAG_FILE)
		{
			CHECK_AND_WRITE_TO_STAT_FILE3( \
					g_storage_stat.total_delete_count, \
					g_storage_stat.success_delete_count, \
					g_storage_stat.last_source_update)
		}

		if (pFileContext->delete_flag & STORAGE_DELETE_FLAG_LINK)
		{
			CHECK_AND_WRITE_TO_STAT_FILE3( \
					g_storage_stat.total_delete_link_count, \
					g_storage_stat.success_delete_link_count, \
					g_storage_stat.last_source_update)
		}

	}

	pClientInfo->total_length = sizeof(TrackerHeader);
	pClientInfo->total_offset = 0;
	pTask->length = pClientInfo->total_length;
	pHeader = (TrackerHeader *)pTask->data;
	pHeader->status = result;
	pHeader->cmd = STORAGE_PROTO_CMD_RESP;
	long2buff(pClientInfo->total_length - sizeof(TrackerHeader), \
			pHeader->pkg_len);

	storage_nio_notify(pTask);
}

static void storage_upload_file_done_callback(struct fast_task_info *pTask, \
			const int err_no)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	TrackerHeader *pHeader;
	int result;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	if (pFileContext->extra_info.upload.if_trunk_file)
	{
		result = trunk_client_trunk_alloc_confirm( \
			&(pFileContext->extra_info.upload.trunk_info), err_no);
		if (err_no != 0)
		{
			result = err_no;
		}
	}
	else
	{
		result = err_no;
	}
	
	if (result == 0)
	{
		result = storage_service_upload_file_done(pTask);
		if (result == 0)
		{
		if (pFileContext->create_flag & STORAGE_CREATE_FLAG_FILE)
		{
			result = storage_binlog_write(\
				pFileContext->timestamp2log, \
				STORAGE_OP_TYPE_SOURCE_CREATE_FILE, \
				pFileContext->fname2log);
		}
		}
	}

	if (result == 0)
	{
		int filename_len;
		char *p;

		if (pFileContext->create_flag & STORAGE_CREATE_FLAG_FILE)
		{
			CHECK_AND_WRITE_TO_STAT_FILE3_WITH_BYTES( \
				g_storage_stat.total_upload_count, \
				g_storage_stat.success_upload_count, \
				g_storage_stat.last_source_update, \
				g_storage_stat.total_upload_bytes, \
				g_storage_stat.success_upload_bytes, \
				pFileContext->end - pFileContext->start)
		}

		filename_len = strlen(pFileContext->fname2log);
		pClientInfo->total_length = sizeof(TrackerHeader) + \
					FDFS_GROUP_NAME_MAX_LEN + filename_len;
		p = pTask->data + sizeof(TrackerHeader);
		memcpy(p, g_group_name, FDFS_GROUP_NAME_MAX_LEN);
		p += FDFS_GROUP_NAME_MAX_LEN;
		memcpy(p, pFileContext->fname2log, filename_len);
	}
	else
	{
		pthread_mutex_lock(&stat_count_thread_lock);
		if (pFileContext->create_flag & STORAGE_CREATE_FLAG_FILE)
		{
			g_storage_stat.total_upload_count++;
 			g_storage_stat.total_upload_bytes += \
				pClientInfo->total_offset;
		}
		pthread_mutex_unlock(&stat_count_thread_lock);

		pClientInfo->total_length = sizeof(TrackerHeader);
	}

	pClientInfo->total_offset = 0;
	pTask->length = pClientInfo->total_length;

	pHeader = (TrackerHeader *)pTask->data;
	pHeader->status = result;
	pHeader->cmd = STORAGE_PROTO_CMD_RESP;
	long2buff(pClientInfo->total_length - sizeof(TrackerHeader), \
			pHeader->pkg_len);

	storage_nio_notify(pTask);
}

static void storage_append_file_done_callback(struct fast_task_info *pTask, \
			const int err_no)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	TrackerHeader *pHeader;
	char extra[64];
	int result;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	if (err_no == 0)
	{
		sprintf(extra, INT64_PRINTF_FORMAT" "INT64_PRINTF_FORMAT, \
				pFileContext->start, \
				pFileContext->end - pFileContext->start);
		result = storage_binlog_write_ex(pFileContext->timestamp2log, \
				pFileContext->sync_flag, \
				pFileContext->fname2log, extra);
	}
	else
	{
		result = err_no;
	}

	if (result == 0)
	{
		CHECK_AND_WRITE_TO_STAT_FILE3_WITH_BYTES( \
			g_storage_stat.total_append_count, \
			g_storage_stat.success_append_count, \
			g_storage_stat.last_source_update, \
			g_storage_stat.total_append_bytes, \
			g_storage_stat.success_append_bytes, \
			pFileContext->end - pFileContext->start)
	}
	else
	{
		pthread_mutex_lock(&stat_count_thread_lock);
		g_storage_stat.total_append_count++;
 		g_storage_stat.total_append_bytes += pClientInfo->total_offset;
		pthread_mutex_unlock(&stat_count_thread_lock);
	}

	pClientInfo->total_length = sizeof(TrackerHeader);
	pClientInfo->total_offset = 0;
	pTask->length = pClientInfo->total_length;

	pHeader = (TrackerHeader *)pTask->data;
	pHeader->status = result;
	pHeader->cmd = STORAGE_PROTO_CMD_RESP;
	long2buff(0, pHeader->pkg_len);

	storage_nio_notify(pTask);
}

static void storage_set_metadata_done_callback(struct fast_task_info *pTask, \
			const int err_no)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	TrackerHeader *pHeader;
	int result;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	if (err_no == 0)
	{
		if (pFileContext->sync_flag != '\0')
		{
		result = storage_binlog_write(pFileContext->timestamp2log, \
			pFileContext->sync_flag, pFileContext->fname2log);
		}
		else
		{
			result = err_no;
		}
	}
	else
	{
		result = err_no;
	}

	if (result != 0)
	{
		pthread_mutex_lock(&stat_count_thread_lock);
		g_storage_stat.total_set_meta_count++;
		pthread_mutex_unlock(&stat_count_thread_lock);
	}
	else
	{
		CHECK_AND_WRITE_TO_STAT_FILE3( \
				g_storage_stat.total_set_meta_count, \
				g_storage_stat.success_set_meta_count, \
				g_storage_stat.last_source_update)
	}

	pClientInfo->total_length = sizeof(TrackerHeader);
	pClientInfo->total_offset = 0;
	pTask->length = pClientInfo->total_length;
	pHeader = (TrackerHeader *)pTask->data;
	pHeader->status = result;
	pHeader->cmd = STORAGE_PROTO_CMD_RESP;
	long2buff(pClientInfo->total_length - sizeof(TrackerHeader), \
			pHeader->pkg_len);

	storage_nio_notify(pTask);
}

int storage_service_init()
{
	int result;
	struct storage_nio_thread_data *pThreadData;
	struct storage_nio_thread_data *pDataEnd;
	pthread_t tid;
	pthread_attr_t thread_attr;

	if ((result=init_pthread_lock(&g_storage_thread_lock)) != 0)
	{
		return result;
	}

	if ((result=init_pthread_lock(&path_index_thread_lock)) != 0)
	{
		return result;
	}

	if ((result=init_pthread_lock(&stat_count_thread_lock)) != 0)
	{
		return result;
	}

	if ((result=init_pthread_attr(&thread_attr, g_thread_stack_size)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"init_pthread_attr fail, program exit!", __LINE__);
		return result;
	}

	if ((result=free_queue_init(g_max_connections, g_buff_size, \
                g_buff_size, sizeof(StorageClientInfo))) != 0)
	{
		return result;
	}

	g_nio_thread_data = (struct storage_nio_thread_data *)malloc(sizeof( \
				struct storage_nio_thread_data) * g_work_threads);
	if (g_nio_thread_data == NULL)
	{
		logError("file: "__FILE__", line: %d, " \
			"malloc %d bytes fail, errno: %d, error info: %s", \
			__LINE__, (int)sizeof(struct storage_nio_thread_data) * \
			g_work_threads, errno, STRERROR(errno));
		return errno != 0 ? errno : ENOMEM;
	}

	g_storage_thread_count = 0;
	pDataEnd = g_nio_thread_data + g_work_threads;
	for (pThreadData=g_nio_thread_data; pThreadData<pDataEnd; pThreadData++)
	{
		pThreadData->ev_base = event_base_new();
		if (pThreadData->ev_base == NULL)
		{
			result = errno != 0 ? errno : ENOMEM;
			logError("file: "__FILE__", line: %d, " \
				"event_base_new fail.", __LINE__);
			return result;
		}

		if (pipe(pThreadData->pipe_fds) != 0)
		{
			result = errno != 0 ? errno : EPERM;
			logError("file: "__FILE__", line: %d, " \
				"call pipe fail, " \
				"errno: %d, error info: %s", \
				__LINE__, result, STRERROR(result));
			break;
		}

		if ((result=set_nonblock(pThreadData->pipe_fds[0])) != 0)
		{
			break;
		}

		if ((result=pthread_create(&tid, &thread_attr, \
			work_thread_entrance, pThreadData)) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"create thread failed, startup threads: %d, " \
				"errno: %d, error info: %s", \
				__LINE__, g_storage_thread_count, \
				result, STRERROR(result));
			break;
		}
		else
		{
			if ((result=pthread_mutex_lock(&g_storage_thread_lock)) != 0)
			{
				logError("file: "__FILE__", line: %d, " \
					"call pthread_mutex_lock fail, " \
					"errno: %d, error info: %s", \
					__LINE__, result, STRERROR(result));
			}
			g_storage_thread_count++;
			if ((result=pthread_mutex_unlock(&g_storage_thread_lock)) != 0)
			{
				logError("file: "__FILE__", line: %d, " \
					"call pthread_mutex_lock fail, " \
					"errno: %d, error info: %s", \
					__LINE__, result, STRERROR(result));
			}
		}
	}

	pthread_attr_destroy(&thread_attr);

	last_stat_change_count = g_stat_change_count;
	extra_open_flags = g_disk_rw_direct ? O_DIRECT : 0;

	return result;
}

void storage_service_destroy()
{
	pthread_mutex_destroy(&g_storage_thread_lock);
	pthread_mutex_destroy(&path_index_thread_lock);
	pthread_mutex_destroy(&stat_count_thread_lock);
}

int storage_terminate_threads()
{
        struct storage_nio_thread_data *pThreadData;
        struct storage_nio_thread_data *pDataEnd;
	struct fast_task_info *pTask;
	StorageClientInfo *pClientInfo;
	long task_addr;
        int quit_sock;

	if (g_nio_thread_data != NULL)
	{
		pDataEnd = g_nio_thread_data + g_work_threads;
		quit_sock = 0;

		for (pThreadData=g_nio_thread_data; pThreadData<pDataEnd; \
				pThreadData++)
		{
			quit_sock--;
			pTask = free_queue_pop();
			if (pTask == NULL)
			{
				logError("file: "__FILE__", line: %d, " \
					"malloc task buff failed", \
					__LINE__);
				continue;
			}

			pClientInfo = (StorageClientInfo *)pTask->arg;
			pClientInfo->sock = quit_sock;
			pClientInfo->nio_thread_index = pThreadData - g_nio_thread_data;

			task_addr = (long)pTask;
			if (write(pThreadData->pipe_fds[1], &task_addr, \
					sizeof(task_addr)) != sizeof(task_addr))
			{
				logError("file: "__FILE__", line: %d, " \
					"call write failed, " \
					"errno: %d, error info: %s", \
					__LINE__, errno, STRERROR(errno));
			}
		}
	}

        return 0;
}

void storage_accept_loop(int server_sock)
{
	int incomesock;
	struct sockaddr_in inaddr;
	socklen_t sockaddr_len;
	in_addr_t client_addr;
	char szClientIp[IP_ADDRESS_SIZE];
	long task_addr;
	struct fast_task_info *pTask;
	StorageClientInfo *pClientInfo;
	struct storage_nio_thread_data *pThreadData;

	while (g_continue_flag)
	{
		sockaddr_len = sizeof(inaddr);
		incomesock = accept(server_sock, (struct sockaddr*)&inaddr, \
					&sockaddr_len);
		if (incomesock < 0) //error
		{
			if (!(errno == EINTR || errno == EAGAIN))
			{
				logError("file: "__FILE__", line: %d, " \
					"accept failed, " \
					"errno: %d, error info: %s", \
					__LINE__, errno, STRERROR(errno));
			}

			continue;
		}

		client_addr = getPeerIpaddr(incomesock, \
				szClientIp, IP_ADDRESS_SIZE);
		if (g_allow_ip_count >= 0)
		{
			if (bsearch(&client_addr, g_allow_ip_addrs, \
					g_allow_ip_count, sizeof(in_addr_t), \
					cmp_by_ip_addr_t) == NULL)
			{
				logError("file: "__FILE__", line: %d, " \
					"ip addr %s is not allowed to access", \
					__LINE__, szClientIp);

				close(incomesock);
				continue;
			}
		}

		if (tcpsetnonblockopt(incomesock) != 0)
		{
			close(incomesock);
			continue;
		}

		pTask = free_queue_pop();
		if (pTask == NULL)
		{
			logError("file: "__FILE__", line: %d, " \
				"malloc task buff failed", \
				__LINE__);
			close(incomesock);
			continue;
		}

		pClientInfo = (StorageClientInfo *)pTask->arg;
		pClientInfo->sock = incomesock;
		pClientInfo->stage = FDFS_STORAGE_STAGE_NIO_INIT;
		pClientInfo->nio_thread_index = pClientInfo->sock % g_work_threads;
		pThreadData = g_nio_thread_data + pClientInfo->nio_thread_index;

		strcpy(pTask->client_ip, szClientIp);
		strcpy(pClientInfo->tracker_client_ip, szClientIp);

		task_addr = (long)pTask;
		if (write(pThreadData->pipe_fds[1], &task_addr, \
			sizeof(task_addr)) != sizeof(task_addr))
		{
			close(incomesock);
			free_queue_push(pTask);
			logError("file: "__FILE__", line: %d, " \
				"call write failed, " \
				"errno: %d, error info: %s", \
				__LINE__, errno, STRERROR(errno));
		}
	}
}

void storage_nio_notify(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	struct storage_nio_thread_data *pThreadData;
	long task_addr;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pThreadData = g_nio_thread_data + pClientInfo->nio_thread_index;

	task_addr = (long)pTask;
	if (write(pThreadData->pipe_fds[1], &task_addr, \
		sizeof(task_addr)) != sizeof(task_addr))
	{
		logError("file: "__FILE__", line: %d, " \
			"call write failed, " \
			"errno: %d, error info: %s", \
			__LINE__, errno, STRERROR(errno));
		task_finish_clean_up(pTask);
	}
}

static void *work_thread_entrance(void* arg)
{
	int result;
	struct storage_nio_thread_data *pThreadData;
	struct event ev_notify;

	pThreadData = (struct storage_nio_thread_data *)arg;
	if (g_check_file_duplicate)
	{
		if ((result=fdht_copy_group_array(&(pThreadData->group_array),\
				&g_group_array)) != 0)
		{
			pthread_mutex_lock(&g_storage_thread_lock);
			g_storage_thread_count--;
			pthread_mutex_unlock(&g_storage_thread_lock);
			return NULL;
		}
	}
	
	do
	{
		event_set(&ev_notify, pThreadData->pipe_fds[0], \
			EV_READ | EV_PERSIST, storage_recv_notify_read, NULL);
		if ((result=event_base_set(pThreadData->ev_base, &ev_notify)) != 0)
		{
			logCrit("file: "__FILE__", line: %d, " \
				"event_base_set fail.", __LINE__);
			break;
		}
		if ((result=event_add(&ev_notify, NULL)) != 0)
		{
			logCrit("file: "__FILE__", line: %d, " \
				"event_add fail.", __LINE__);
			break;
		}

		while (g_continue_flag)
		{
			event_base_loop(pThreadData->ev_base, 0);
		}
	} while (0);

	event_base_free(pThreadData->ev_base);

	if (g_check_file_duplicate)
	{
		if (g_keep_alive)
		{
			fdht_disconnect_all_servers(&(pThreadData->group_array));
		}

		fdht_free_group_array(&(pThreadData->group_array));
	}

	if ((result=pthread_mutex_lock(&g_storage_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_lock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, STRERROR(result));
	}
	g_storage_thread_count--;
	if ((result=pthread_mutex_unlock(&g_storage_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_lock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, STRERROR(result));
	}

	logDebug("file: "__FILE__", line: %d, " \
		"nio thread exited, thread count: %d", \
		__LINE__, g_storage_thread_count);

	return NULL;
}

void storage_get_store_path(const char *filename, const int filename_len, \
		int *sub_path_high, int *sub_path_low)
{
	int n;
	int result;

	if (g_file_distribute_path_mode == FDFS_FILE_DIST_PATH_ROUND_ROBIN)
	{
		*sub_path_high = g_dist_path_index_high;
		*sub_path_low = g_dist_path_index_low;

		if (++g_dist_write_file_count >= g_file_distribute_rotate_count)
		{
			g_dist_write_file_count = 0;
	
			if ((result=pthread_mutex_lock( \
					&path_index_thread_lock)) != 0)
			{
				logError("file: "__FILE__", line: %d, " \
					"call pthread_mutex_lock fail, " \
					"errno: %d, error info: %s", \
					__LINE__, result, STRERROR(result));
			}

			++g_dist_path_index_low;
			if (g_dist_path_index_low >= g_subdir_count_per_path)
			{  //rotate
				g_dist_path_index_high++;
				if (g_dist_path_index_high >= \
					g_subdir_count_per_path)  //rotate
				{
					g_dist_path_index_high = 0;
				}
				g_dist_path_index_low = 0;
			}

			++g_stat_change_count;

			if ((result=pthread_mutex_unlock( \
					&path_index_thread_lock)) != 0)
			{
				logError("file: "__FILE__", line: %d, " \
					"call pthread_mutex_unlock fail, " \
					"errno: %d, error info: %s", \
					__LINE__, result, STRERROR(result));
			}
		}
	}  //random
	else
	{
		n = PJWHash(filename, filename_len) % (1 << 16);
		*sub_path_high = ((n >> 8) & 0xFF) % g_subdir_count_per_path;
		*sub_path_low = (n & 0xFF) % g_subdir_count_per_path;

	}
}

static int storage_gen_filename(StorageClientInfo *pClientInfo, \
		const int64_t file_size, const int crc32, \
		const char *szFormattedExt, const int ext_name_len, \
		const time_t timestamp, char *filename, int *filename_len)
{
	char buff[sizeof(int) * 5];
	char encoded[sizeof(int) * 8 + 1];
	char szStorageIp[IP_ADDRESS_SIZE];
	int len;
	int r;
	int sub_path_high;
	int sub_path_low;
	in_addr_t server_ip;
	int64_t masked_file_size;

	server_ip = getSockIpaddr(pClientInfo->sock, \
			szStorageIp, IP_ADDRESS_SIZE);

	int2buff(htonl(server_ip), buff);
	int2buff(timestamp, buff+sizeof(int));
	if ((file_size >> 32) != 0)
	{
		masked_file_size = file_size;
	}
	else
	{
		r = rand();
		if ((r & 0x80000000) == 0)
		{
			r |= 0x80000000;
		}

		masked_file_size = (((int64_t)r) << 32 ) | file_size;
	}
	long2buff(masked_file_size, buff+sizeof(int)*2);
	int2buff(crc32, buff+sizeof(int)*4);

	base64_encode_ex(&g_base64_context, buff, sizeof(int) * 5, encoded, \
			filename_len, false);

	storage_get_store_path(encoded, *filename_len, \
				&sub_path_high, &sub_path_low);

	len = sprintf(filename, STORAGE_DATA_DIR_FORMAT"/" \
			STORAGE_DATA_DIR_FORMAT"/", \
			sub_path_high, sub_path_low);
	memcpy(filename+len, encoded, *filename_len);
	memcpy(filename+len+(*filename_len), szFormattedExt, ext_name_len);
	*filename_len += len + ext_name_len;
	*(filename + (*filename_len)) = '\0';

	return 0;
}

static int storage_sort_metadata_buff(char *meta_buff, const int meta_size)
{
	FDFSMetaData *meta_list;
	int meta_count;
	int meta_bytes;
	int result;

	meta_list = fdfs_split_metadata(meta_buff, &meta_count, &result);
	if (meta_list == NULL)
	{
		return result;
	}

	qsort((void *)meta_list, meta_count, sizeof(FDFSMetaData), \
		metadata_cmp_by_name);

	fdfs_pack_metadata(meta_list, meta_count, meta_buff, &meta_bytes);
	free(meta_list);

	return 0;
}

static void storage_format_ext_name(const char *file_ext_name, \
		char *szFormattedExt)
{
	int i;
	int ext_name_len;
	int pad_len;
	char *p;

	ext_name_len = strlen(file_ext_name);
	if (ext_name_len == 0)
	{
		pad_len = FDFS_FILE_EXT_NAME_MAX_LEN + 1;
	}
	else
	{
		pad_len = FDFS_FILE_EXT_NAME_MAX_LEN - ext_name_len;
	}

	p = szFormattedExt;
	for (i=0; i<pad_len; i++)
	{
		*p++ = '0' + (int)(10.0 * (double)rand() / RAND_MAX);
	}

	if (ext_name_len > 0)
	{
		*p++ = '.';
		memcpy(p, file_ext_name, ext_name_len);
		p += ext_name_len;
	}
	*p = '\0';
}
 
static int storage_get_filename(StorageClientInfo *pClientInfo, \
		const int start_time, const int store_path_index, \
		const int64_t file_size, const int crc32, \
		const char *szFormattedExt, char *filename, \
		int *filename_len, char *full_filename)
{
	int i;
	int result;

	for (i=0; i<10; i++)
	{
		if ((result=storage_gen_filename(pClientInfo, file_size, \
			crc32, szFormattedExt, FDFS_FILE_EXT_NAME_MAX_LEN+1, \
			start_time, filename, filename_len)) != 0)
		{
			return result;
		}

		sprintf(full_filename, "%s/data/%s", \
			g_store_paths[store_path_index], filename);
		if (!fileExists(full_filename))
		{
			break;
		}

		*full_filename = '\0';
	}

	if (*full_filename == '\0')
	{
		logError("file: "__FILE__", line: %d, " \
			"Can't generate uniq filename", __LINE__);
		*filename = '\0';
		*filename_len = 0;
		return ENOENT;
	}

	return 0;
}

static int storage_client_create_link_wrapper(struct fast_task_info *pTask, \
		const char *master_filename, \
		const char *src_filename, const int src_filename_len, \
		const char *src_file_sig, const int src_file_sig_len, \
		const char *group_name, const char *prefix_name, \
		const char *file_ext_name, \
		char *remote_filename, int *filename_len)
{
	int result;
	int src_store_path_index;
	TrackerServerInfo trackerServer;
	TrackerServerInfo storageServer;
	TrackerServerInfo *pStorageServer;
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	SourceFileInfo sourceFileInfo;
	bool bCreateDirectly;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	if ((result=tracker_get_connection_r(&trackerServer)) != 0)
	{
		return result;
	}

	if (strcmp(group_name, g_group_name) != 0)
	{
		pStorageServer = NULL;
		bCreateDirectly = false;
	}
	else
	{
		pStorageServer = &storageServer;
		result = tracker_query_storage_update(&trackerServer, \
				pStorageServer, group_name, src_filename);
		if (result != 0)
		{
			tracker_disconnect_server(&trackerServer);
			return result;
		}

		if (is_local_host_ip(pStorageServer->ip_addr))
		{
			bCreateDirectly = true;
		}
		else
		{
			bCreateDirectly = false;
			if ((result=tracker_connect_server(pStorageServer)) != 0)
			{
				tracker_disconnect_server(&trackerServer);
				return result;
			}
		}
	}

	if (bCreateDirectly)
	{
		sourceFileInfo.src_file_sig_len = src_file_sig_len;
		memcpy(sourceFileInfo.src_file_sig, src_file_sig, \
			src_file_sig_len);
		*(sourceFileInfo.src_file_sig + src_file_sig_len) = '\0';

		*filename_len = src_filename_len;
		if ((result=storage_split_filename_ex(src_filename, \
			filename_len, sourceFileInfo.src_true_filename, \
			&src_store_path_index)) != 0)
		{
			tracker_disconnect_server(&trackerServer);
			return result;
		}

		result = storage_create_link_core(pTask, \
			src_store_path_index, &sourceFileInfo, src_filename, \
			master_filename, strlen(master_filename), \
			prefix_name, file_ext_name, \
			remote_filename, filename_len);
	}
	else
	{
		result = storage_client_create_link(&trackerServer, \
				pStorageServer, master_filename, \
				src_filename, src_filename_len, \
				src_file_sig, src_file_sig_len, \
				group_name, prefix_name, \
				file_ext_name, remote_filename, filename_len);
		if (pStorageServer != NULL)
		{
			fdfs_quit(pStorageServer);
			tracker_disconnect_server(pStorageServer);
		}
	}

	fdfs_quit(&trackerServer);
	tracker_disconnect_server(&trackerServer);

	return result;
}

static int storage_service_upload_file_done(struct fast_task_info *pTask)
{
	int result;
	int filename_len;
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	int64_t file_size;
	int64_t file_size_in_name;
	time_t end_time;
	char new_fname2log[128];
	char new_full_filename[MAX_PATH_SIZE+64];
	char new_filename[128];
	int new_filename_len;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);
	file_size = pFileContext->end - pFileContext->start;

	*new_full_filename = '\0';
	*new_filename = '\0';
	new_filename_len = 0;

	if (pFileContext->extra_info.upload.if_appender_file)
	{
		end_time = time(NULL);
		file_size_in_name = FDFS_APPENDER_FILE_SIZE;
	}
	else if (pFileContext->extra_info.upload.if_trunk_file)
	{
		end_time = pFileContext->extra_info.upload.start_time;
		file_size_in_name = FDFS_TRUNK_FILE_SIZE | file_size;
	}
	else
	{
		end_time = time(NULL);
		file_size_in_name = file_size;
	}

	if ((result=storage_get_filename(pClientInfo, end_time, \
		pFileContext->extra_info.upload.store_path_index, \
		file_size_in_name, pFileContext->crc32, \
		pFileContext->extra_info.upload.formatted_ext_name, \
		new_filename, &new_filename_len, new_full_filename)) != 0)
	{
		unlink(pFileContext->filename);
		return result;
	}

	sprintf(new_fname2log, "%c"STORAGE_DATA_DIR_FORMAT"/%s", \
		FDFS_STORAGE_STORE_PATH_PREFIX_CHAR, \
		pFileContext->extra_info.upload.store_path_index, \
		new_filename);

	if (pFileContext->extra_info.upload.if_trunk_file)
	{
		char trunk_buff[FDFS_TRUNK_FILE_INFO_LEN + 1];
		trunk_file_info_encode(&(pFileContext->extra_info.upload. \
					trunk_info.file), trunk_buff);

		sprintf(new_fname2log + FDFS_LOGIC_FILE_PATH_LEN \
			+ FDFS_FILENAME_BASE64_LENGTH, "%s%s", trunk_buff, \
			new_filename + FDFS_TRUE_FILE_PATH_LEN + \
			FDFS_FILENAME_BASE64_LENGTH);
	}
	else if (rename(pFileContext->filename, new_full_filename) != 0)
	{
		result = errno != 0 ? errno : EPERM;
		logError("file: "__FILE__", line: %d, " \
			"rename %s to %s fail, " \
			"errno: %d, error info: %s", __LINE__, \
			pFileContext->filename, new_full_filename, \
			result, STRERROR(result));

		unlink(pFileContext->filename);
		return result;
	}

	if ((!pFileContext->extra_info.upload.if_gen_filename) && \
		(!g_check_file_duplicate))
	{   //upload slave file
		if (symlink(new_full_filename, pFileContext->filename) != 0)
		{
			result = errno != 0 ? errno : ENOENT;
			logError("file: "__FILE__", line: %d, " \
				"link file %s to %s fail, " \
				"errno: %d, error info: %s", __LINE__, \
				new_full_filename, pFileContext->filename, \
				result, STRERROR(result));

			unlink(new_full_filename);
			return result;
		}
	}
	else
	{
		strcpy(pFileContext->filename, new_full_filename);
		strcpy(pFileContext->fname2log, new_fname2log);
	}

	pFileContext->timestamp2log = end_time;

	if (g_check_file_duplicate)
	{
		GroupArray *pGroupArray;
		char value[128];
		FDHTKeyInfo key_info;
		char *pValue;
		int value_len;
		int nSigLen;
		char szFileSig[FILE_SIGNATURE_SIZE];

		memset(&key_info, 0, sizeof(key_info));
		key_info.namespace_len = g_namespace_len;
		memcpy(key_info.szNameSpace, g_key_namespace, g_namespace_len);

		pGroupArray=&((g_nio_thread_data+pClientInfo->nio_thread_index)\
				->group_array);

		STORAGE_GEN_FILE_SIGNATURE(file_size, \
				pFileContext->file_hash_codes, szFileSig)

		nSigLen = FILE_SIGNATURE_SIZE;
		key_info.obj_id_len = nSigLen;
		memcpy(key_info.szObjectId, szFileSig, nSigLen);
		key_info.key_len = sizeof(FDHT_KEY_NAME_FILE_ID) - 1;
		memcpy(key_info.szKey, FDHT_KEY_NAME_FILE_ID, \
				sizeof(FDHT_KEY_NAME_FILE_ID) - 1);

		pValue = value;
		value_len = sizeof(value) - 1;
		result = fdht_get_ex1(pGroupArray, g_keep_alive, \
				&key_info, FDHT_EXPIRES_NONE, \
				&pValue, &value_len, malloc);
		if (result == 0)
		{   //exists
			char *pGroupName;
			char *pSrcFilename;
			char *pSeperator;

			if (unlink(pFileContext->filename) != 0)
			{
				result = errno != 0 ? errno : EPERM;
				logError("file: "__FILE__", line: %d, "\
					"unlink %s fail, errno: %d, " \
					"error info: %s", \
					__LINE__, pFileContext->filename, \
					result, STRERROR(result));

				return result;
			}

			*(value + value_len) = '\0';

			pSeperator = strchr(value, '/');
			if (pSeperator == NULL)
			{
				logError("file: "__FILE__", line: %d, "\
					"value %s is invalid", \
					__LINE__, value);

				return EINVAL;
			}

			*pSeperator = '\0';
			pGroupName = value;
			pSrcFilename = pSeperator + 1;
			result = storage_client_create_link_wrapper(pTask, \
				pFileContext->extra_info.upload.master_filename, \
				pSrcFilename, value_len-(pSrcFilename-value),\
				key_info.szObjectId, key_info.obj_id_len, \
				pGroupName, \
				pFileContext->extra_info.upload.prefix_name, \
				pFileContext->extra_info.upload.file_ext_name,\
				pFileContext->fname2log, &filename_len);


			pFileContext->create_flag = STORAGE_CREATE_FLAG_LINK;
			return result;
		}
		else if (result == ENOENT)
		{
			char src_filename[128];
			FDHTKeyInfo ref_count_key;

			filename_len = sprintf(src_filename, "%s", \
					pFileContext->fname2log);
			value_len = sprintf(value, "%s/%s", \
					g_group_name, pFileContext->fname2log);
			if ((result=fdht_set_ex(pGroupArray, g_keep_alive, \
						&key_info, FDHT_EXPIRES_NEVER, \
						value, value_len)) != 0)
			{
				logError("file: "__FILE__", line: %d, "\
					"client ip: %s, fdht_set fail,"\
					"errno: %d, error info: %s", \
					__LINE__, pTask->client_ip, \
					result, STRERROR(result));

				unlink(pFileContext->filename);
				return result;
			}

			memcpy(&ref_count_key, &key_info, sizeof(FDHTKeyInfo));
			ref_count_key.obj_id_len = value_len;
			memcpy(ref_count_key.szObjectId, value, value_len);
			ref_count_key.key_len = sizeof(FDHT_KEY_NAME_REF_COUNT) - 1;
			memcpy(ref_count_key.szKey, FDHT_KEY_NAME_REF_COUNT, \
					ref_count_key.key_len);
			if ((result=fdht_set_ex(pGroupArray, g_keep_alive, \
				&ref_count_key, FDHT_EXPIRES_NEVER, "0", 1)) != 0)
			{
				logError("file: "__FILE__", line: %d, "\
					"client ip: %s, fdht_set fail,"\
					"errno: %d, error info: %s", \
					__LINE__, pTask->client_ip, \
					result, STRERROR(result));

				unlink(pFileContext->filename);
				return result;
			}


			result = storage_binlog_write(pFileContext->timestamp2log, \
					STORAGE_OP_TYPE_SOURCE_CREATE_FILE, \
					src_filename);
			if (result != 0)
			{
				unlink(pFileContext->filename);
				return result;
			}

			result = storage_client_create_link_wrapper(pTask, \
				pFileContext->extra_info.upload.master_filename, \
				src_filename, filename_len, szFileSig, nSigLen,\
				g_group_name, pFileContext->extra_info.upload.prefix_name, \
				pFileContext->extra_info.upload.file_ext_name, \
				pFileContext->fname2log, &filename_len);

			if (result != 0)
			{
				fdht_delete_ex(pGroupArray, g_keep_alive, &key_info);
				fdht_delete_ex(pGroupArray, g_keep_alive, &ref_count_key);

				unlink(pFileContext->filename);
			}

			pFileContext->create_flag = STORAGE_CREATE_FLAG_LINK;
			return result;
		}
		else //error
		{
			logError("file: "__FILE__", line: %d, " \
				"fdht_get fail, " \
				"errno: %d, error info: %s", \
				__LINE__, result, STRERROR(errno));

			unlink(pFileContext->filename);
			return result;
		}
	}

	if (!pFileContext->extra_info.upload.if_gen_filename) //upload slave file
	{
		result = storage_binlog_write( \
				pFileContext->timestamp2log, \
				STORAGE_OP_TYPE_SOURCE_CREATE_FILE, \
				new_fname2log);
		if (result == 0)
		{
			result = storage_binlog_write( \
				pFileContext->timestamp2log, \
				STORAGE_OP_TYPE_SOURCE_CREATE_LINK, \
				pFileContext->fname2log);
		}

		if (result != 0)
		{
			unlink(new_full_filename);
			unlink(pFileContext->filename);
			return result;
		}

		pFileContext->create_flag = STORAGE_CREATE_FLAG_LINK;
	}
	else
	{
		pFileContext->create_flag = STORAGE_CREATE_FLAG_FILE;
	}

	return 0;
}

static int storage_service_do_create_link(struct fast_task_info *pTask, \
		const int store_path_index, const SourceFileInfo *pSrcFileInfo,\
		const int64_t file_size, const char *master_filename, \
		const char *prefix_name, const char *file_ext_name,  \
		char *filename, int *filename_len)
{
	int result;
	int crc32;
	StorageClientInfo *pClientInfo;
	char src_full_filename[MAX_PATH_SIZE+64];
	char full_filename[MAX_PATH_SIZE+64];

	pClientInfo = (StorageClientInfo *)pTask->arg;

	if (*filename_len == 0)
	{
		char formatted_ext_name[FDFS_FILE_EXT_NAME_MAX_LEN + 2];

		storage_format_ext_name(file_ext_name, formatted_ext_name);
		crc32 = rand();
		if ((result=storage_get_filename(pClientInfo, time(NULL), \
			store_path_index, file_size, crc32, \
			formatted_ext_name, filename, filename_len, \
			full_filename)) != 0)
		{
			return result;
		}
	}
	else
	{
		sprintf(full_filename, "%s/data/%s", \
			g_store_paths[store_path_index], \
			filename);
	}

	sprintf(src_full_filename, "%s/data/%s", \
			g_store_paths[store_path_index], \
			pSrcFileInfo->src_true_filename);
	if (symlink(src_full_filename, full_filename) != 0)
	{
		result = errno != 0 ? errno : ENOENT;
		logError("file: "__FILE__", line: %d, " \
			"link file %s to %s fail, " \
			"errno: %d, error info: %s", __LINE__, \
			src_full_filename, full_filename, \
			result, STRERROR(result));
		*filename = '\0';
		*filename_len = 0;
		return result;
	}

	*filename_len=sprintf(full_filename, "%c"STORAGE_DATA_DIR_FORMAT"/%s", \
			FDFS_STORAGE_STORE_PATH_PREFIX_CHAR, \
			store_path_index, filename);
	memcpy(filename, full_filename, (*filename_len) + 1);

	if (g_check_file_duplicate)
	{
		GroupArray *pGroupArray;
		FDHTKeyInfo key_info;
		char value[128];
		int value_len;

		memset(&key_info, 0, sizeof(key_info));
		key_info.namespace_len = g_namespace_len;
		memcpy(key_info.szNameSpace, g_key_namespace, g_namespace_len);

		pGroupArray=&((g_nio_thread_data + pClientInfo->nio_thread_index) \
				->group_array);

		key_info.obj_id_len = snprintf(key_info.szObjectId, \
			sizeof(key_info.szObjectId), \
			"%s/%c"STORAGE_DATA_DIR_FORMAT"/%s", \
			g_group_name, FDFS_STORAGE_STORE_PATH_PREFIX_CHAR, \
			store_path_index, pSrcFileInfo->src_true_filename);

		key_info.key_len = sizeof(FDHT_KEY_NAME_REF_COUNT) - 1;
		memcpy(key_info.szKey, FDHT_KEY_NAME_REF_COUNT, \
			key_info.key_len);
		value_len = sizeof(value) - 1;
		if ((result=fdht_inc_ex(pGroupArray, g_keep_alive, &key_info, \
			FDHT_EXPIRES_NEVER, 1, value, &value_len)) != 0)
		{
			logWarning("file: "__FILE__", line: %d, " \
				"client ip: %s, fdht_inc fail," \
				"errno: %d, error info: %s", \
				__LINE__, pTask->client_ip, \
				result, STRERROR(result));
		}
		else
		{
		//*(value + value_len) = '\0';

		key_info.obj_id_len = snprintf(key_info.szObjectId, \
			sizeof(key_info.szObjectId), \
			"%s/%s", g_group_name, filename);
		key_info.key_len = sizeof(FDHT_KEY_NAME_FILE_SIG) - 1;
		memcpy(key_info.szKey, FDHT_KEY_NAME_FILE_SIG, \
			key_info.key_len);
		if ((result=fdht_set_ex(pGroupArray, g_keep_alive, \
			&key_info, FDHT_EXPIRES_NEVER, \
			pSrcFileInfo->src_file_sig, \
			pSrcFileInfo->src_file_sig_len)) != 0)
		{
			logWarning("file: "__FILE__", line: %d, " \
				"client ip: %s, fdht_set fail," \
				"errno: %d, error info: %s", \
				__LINE__, pTask->client_ip, \
				result, STRERROR(result));
		}

		/*
		logInfo("create link, counter=%s, object_id=%s(%d), key=%s, file_sig_len=(%d)", \
			value, key_info.szObjectId, key_info.obj_id_len, \
			FDHT_KEY_NAME_FILE_SIG, \
			pSrcFileInfo->src_file_sig_len);
		*/
		}
	}

	return 0;
}

static int storage_do_set_metadata(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	FDFSMetaData *old_meta_list;
	FDFSMetaData *new_meta_list;
	FDFSMetaData *all_meta_list;
	FDFSMetaData *pOldMeta;
	FDFSMetaData *pNewMeta;
	FDFSMetaData *pAllMeta;
	FDFSMetaData *pOldMetaEnd;
	FDFSMetaData *pNewMetaEnd;
	char *meta_buff;
	char *file_buff;
	char *all_meta_buff;
	int64_t file_bytes;
	int meta_bytes;
	int old_meta_count;
	int new_meta_count;
	int all_meta_bytes;
	int result;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	pFileContext->sync_flag = '\0';
	meta_buff = pFileContext->extra_info.setmeta.meta_buff;
	meta_bytes = pFileContext->extra_info.setmeta.meta_bytes;

	do
	{
	if (pFileContext->extra_info.setmeta.op_flag == \
			STORAGE_SET_METADATA_FLAG_OVERWRITE)
	{
		if (meta_bytes == 0)
		{
			if (!fileExists(pFileContext->filename))
			{
				result = 0;
				break;
			}

			pFileContext->sync_flag = STORAGE_OP_TYPE_SOURCE_DELETE_FILE;
			if (unlink(pFileContext->filename) != 0)
			{
				logError("file: "__FILE__", line: %d, " \
					"client ip: %s, delete file %s fail," \
					"errno: %d, error info: %s", __LINE__, \
					pTask->client_ip, pFileContext->filename, \
					errno, STRERROR(errno));
				result = errno != 0 ? errno : EPERM;
			}
			else
			{
				result = 0;
			}

			break;
		}

		if ((result=storage_sort_metadata_buff(meta_buff, \
				meta_bytes)) != 0)
		{
			break;
		}

		if (fileExists(pFileContext->filename))
		{
			pFileContext->sync_flag = STORAGE_OP_TYPE_SOURCE_UPDATE_FILE;
		}
		else
		{
			pFileContext->sync_flag = STORAGE_OP_TYPE_SOURCE_CREATE_FILE;
		}

		result = writeToFile(pFileContext->filename, meta_buff, meta_bytes);
		break;
	}

	if (meta_bytes == 0)
	{
		result = 0;
		break;
	}

	result = getFileContent(pFileContext->filename, &file_buff, &file_bytes);
	if (result == ENOENT)
	{
		if (meta_bytes == 0)
		{
			result = 0;
			break;
		}

		if ((result=storage_sort_metadata_buff(meta_buff, \
				meta_bytes)) != 0)
		{
			break;
		}

		pFileContext->sync_flag = STORAGE_OP_TYPE_SOURCE_CREATE_FILE;
		result = writeToFile(pFileContext->filename, meta_buff, meta_bytes);
		break;
	}
	else if (result != 0)
	{
		break;
	}

	old_meta_list = fdfs_split_metadata(file_buff, &old_meta_count, &result);
	if (old_meta_list == NULL)
	{
		free(file_buff);
		break;
	}

	new_meta_list = fdfs_split_metadata(meta_buff, &new_meta_count, &result);
	if (new_meta_list == NULL)
	{
		free(file_buff);
		free(old_meta_list);
		break;
	}

	all_meta_list = (FDFSMetaData *)malloc(sizeof(FDFSMetaData) * \
				(old_meta_count + new_meta_count));
	if (all_meta_list == NULL)
	{
		free(file_buff);
		free(old_meta_list);
		free(new_meta_list);

		logError("file: "__FILE__", line: %d, " \
			"malloc %d bytes fail", __LINE__, \
			(int)sizeof(FDFSMetaData) \
			 * (old_meta_count + new_meta_count));
		result = errno != 0 ? errno : ENOMEM;
		break;
	}

	qsort((void *)new_meta_list, new_meta_count, sizeof(FDFSMetaData), \
		metadata_cmp_by_name);

	pOldMetaEnd = old_meta_list + old_meta_count;
	pNewMetaEnd = new_meta_list + new_meta_count;
	pOldMeta = old_meta_list;
	pNewMeta = new_meta_list;
	pAllMeta = all_meta_list;
	while (pOldMeta < pOldMetaEnd && pNewMeta < pNewMetaEnd)
	{
		result = strcmp(pOldMeta->name, pNewMeta->name);
		if (result < 0)
		{
			memcpy(pAllMeta, pOldMeta, sizeof(FDFSMetaData));
			pOldMeta++;
		}
		else if (result == 0)
		{
			memcpy(pAllMeta, pNewMeta, sizeof(FDFSMetaData));
			pOldMeta++;
			pNewMeta++;
		}
		else  //result > 0
		{
			memcpy(pAllMeta, pNewMeta, sizeof(FDFSMetaData));
			pNewMeta++;
		}

		pAllMeta++;
	}

	while (pOldMeta < pOldMetaEnd)
	{
		memcpy(pAllMeta, pOldMeta, sizeof(FDFSMetaData));
		pOldMeta++;
		pAllMeta++;
	}

	while (pNewMeta < pNewMetaEnd)
	{
		memcpy(pAllMeta, pNewMeta, sizeof(FDFSMetaData));
		pNewMeta++;
		pAllMeta++;
	}

	free(file_buff);
	free(old_meta_list);
	free(new_meta_list);

	all_meta_buff = fdfs_pack_metadata(all_meta_list, \
			pAllMeta - all_meta_list, NULL, &all_meta_bytes);
	free(all_meta_list);
	if (all_meta_buff == NULL)
	{
		result = errno != 0 ? errno : ENOMEM;
		break;
	}

	pFileContext->sync_flag = STORAGE_OP_TYPE_SOURCE_UPDATE_FILE;
	result = writeToFile(pFileContext->filename, all_meta_buff, all_meta_bytes);

	free(all_meta_buff);
	} while (0);

	storage_set_metadata_done_callback(pTask, result);
	return result;
}

/**
8 bytes: filename length
8 bytes: meta data size
1 bytes: operation flag, 
     'O' for overwrite all old metadata
     'M' for merge, insert when the meta item not exist, otherwise update it
FDFS_GROUP_NAME_MAX_LEN bytes: group_name
filename
meta data bytes: each meta data seperated by \x01,
		 name and value seperated by \x02
**/
static int storage_server_set_metadata(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	int64_t nInPackLen;
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	char filename[128];
	char true_filename[128];
	char *p;
	char *meta_buff;
	int meta_bytes;
	int filename_len;
	int true_filename_len;
	int result;
	int store_path_index;
	struct stat stat_buf;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	nInPackLen = pClientInfo->total_length - sizeof(TrackerHeader);

	if (nInPackLen <= 2 * FDFS_PROTO_PKG_LEN_SIZE + 1 + \
			FDFS_GROUP_NAME_MAX_LEN)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			INT64_PRINTF_FORMAT" is not correct, " \
			"expect length > %d", \
			__LINE__, STORAGE_PROTO_CMD_SET_METADATA, \
			pTask->client_ip,  nInPackLen, \
			2 * FDFS_PROTO_PKG_LEN_SIZE + 1 \
			+ FDFS_GROUP_NAME_MAX_LEN);

		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	if (nInPackLen + sizeof(TrackerHeader) >= pTask->size)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			INT64_PRINTF_FORMAT" is not correct, " \
			"expect length < %d", \
			__LINE__, STORAGE_PROTO_CMD_SET_METADATA, \
			pTask->client_ip,  nInPackLen, \
			pTask->size - (int)sizeof(TrackerHeader));

		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	p = pTask->data + sizeof(TrackerHeader);
	filename_len = buff2long(p);
	p += FDFS_PROTO_PKG_LEN_SIZE;
	meta_bytes = buff2long(p);
	p += FDFS_PROTO_PKG_LEN_SIZE;
	if (filename_len <= 0 || filename_len >= sizeof(filename))
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, invalid filename length: %d", \
			__LINE__, pTask->client_ip, filename_len);

		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	pFileContext->extra_info.setmeta.op_flag = *p++;
	if (pFileContext->extra_info.setmeta.op_flag != \
		STORAGE_SET_METADATA_FLAG_OVERWRITE && \
	    pFileContext->extra_info.setmeta.op_flag != \
		STORAGE_SET_METADATA_FLAG_MERGE)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, " \
			"invalid operation flag: 0x%02X", \
			__LINE__, pTask->client_ip, \
			pFileContext->extra_info.setmeta.op_flag);

		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	if (meta_bytes < 0 || meta_bytes != nInPackLen - \
		(2 * FDFS_PROTO_PKG_LEN_SIZE + 1 + \
		 FDFS_GROUP_NAME_MAX_LEN + filename_len))
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, invalid meta bytes: %d", \
			__LINE__, pTask->client_ip, meta_bytes);

		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	memcpy(group_name, p, FDFS_GROUP_NAME_MAX_LEN);
	*(group_name + FDFS_GROUP_NAME_MAX_LEN) = '\0';
	p += FDFS_GROUP_NAME_MAX_LEN;
	if (strcmp(group_name, g_group_name) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, group_name: %s " \
			"not correct, should be: %s", \
			__LINE__, pTask->client_ip, \
			group_name, g_group_name);
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	memcpy(filename, p, filename_len);
	*(filename + filename_len) = '\0';
	p += filename_len;

	true_filename_len = filename_len;
	if ((result=storage_split_filename_ex(filename, \
		&true_filename_len, true_filename, &store_path_index)) != 0)
	{
		pClientInfo->total_length = sizeof(TrackerHeader);
		return result;
	}
	if ((result=fdfs_check_data_filename(true_filename, \
			true_filename_len)) != 0)
	{
		pClientInfo->total_length = sizeof(TrackerHeader);
		return result;
	}

	meta_buff = p;
	*(meta_buff + meta_bytes) = '\0';

	if ((result=trunk_file_lstat(store_path_index, true_filename, \
			true_filename_len, &stat_buf, \
			&(pFileContext->extra_info.upload.trunk_info))) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, call lstat logic file: %s fail, " \
			"errno: %d, error info: %s", \
			__LINE__, pTask->client_ip, filename, \
			result, STRERROR(result));

		pClientInfo->total_length = sizeof(TrackerHeader);
		return result;
	}

	pFileContext->timestamp2log = time(NULL);
	sprintf(pFileContext->filename, "%s/data/%s%s", \
		g_store_paths[store_path_index], true_filename, \
		STORAGE_META_FILE_EXT);
	sprintf(pFileContext->fname2log,"%s%s", \
		filename, STORAGE_META_FILE_EXT);

	pClientInfo->deal_func = storage_do_set_metadata;
	pFileContext->extra_info.setmeta.meta_buff = meta_buff;
	pFileContext->extra_info.setmeta.meta_bytes = meta_bytes;

	pFileContext->dio_thread_index = storage_dio_get_thread_index( \
		pTask, store_path_index, FDFS_STORAGE_FILE_OP_WRITE);

	if ((result=storage_dio_queue_push(pTask)) != 0)
	{
		pClientInfo->total_length = sizeof(TrackerHeader);
		return result;
	}

	return STORAGE_STATUE_DEAL_FILE;
}

/**
IP_ADDRESS_SIZE bytes: tracker client ip address
**/
static int storage_server_report_client_ip(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	char *tracker_client_ip;
	int64_t nInPackLen;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	nInPackLen = pClientInfo->total_length - sizeof(TrackerHeader);
	pClientInfo->total_length = sizeof(TrackerHeader);
	if (nInPackLen != IP_ADDRESS_SIZE)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			INT64_PRINTF_FORMAT" is not correct, " \
			"expect length: %d", __LINE__, \
			STORAGE_PROTO_CMD_REPORT_CLIENT_IP, \
			pTask->client_ip,  \
			nInPackLen, IP_ADDRESS_SIZE);
		return EINVAL;
	}

	tracker_client_ip = pTask->data + sizeof(TrackerHeader);
	*(tracker_client_ip + (IP_ADDRESS_SIZE - 1)) = '\0';
	strcpy(pClientInfo->tracker_client_ip, tracker_client_ip);

	logInfo("file: "__FILE__", line: %d, " \
			"client ip: %s, tracker client ip is %s", \
			__LINE__, pTask->client_ip, tracker_client_ip);

	return 0;
}

/**
N bytes: binlog
**/
static int storage_server_trunk_sync_binlog(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	char *binlog_buff;
	int64_t nInPackLen;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	nInPackLen = pClientInfo->total_length - sizeof(TrackerHeader);
	pClientInfo->total_length = sizeof(TrackerHeader);
	if (nInPackLen == 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			INT64_PRINTF_FORMAT" is not correct", __LINE__, \
			STORAGE_PROTO_CMD_TRUNK_SYNC_BINLOG, \
			pTask->client_ip,  nInPackLen);
		return EINVAL;
	}

	if (!g_if_use_trunk_file)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, invalid command: %d, " \
			"because i don't use trunk file!", \
			__LINE__, pTask->client_ip, \
			STORAGE_PROTO_CMD_TRUNK_SYNC_BINLOG);
		return EINVAL;
	}

	if (g_if_trunker_self)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, invalid command: %d, " \
			"because i am the TRUNK server!", \
			__LINE__, pTask->client_ip, \
			STORAGE_PROTO_CMD_TRUNK_SYNC_BINLOG);
		return EINVAL;
	}

	binlog_buff = pTask->data + sizeof(TrackerHeader);
	return trunk_binlog_write_buffer(binlog_buff, nInPackLen);
}

/**
FDFS_GROUP_NAME_MAX_LEN bytes: group_name
filename
**/
static int storage_server_query_file_info(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	char *in_buff;
	char *filename;
	char *p;
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	char true_filename[128];
	char src_filename[MAX_PATH_SIZE + 128];
	char decode_buff[64];
	struct stat file_stat;
	FDFSTrunkFullInfo trunkInfo;
	int64_t nInPackLen;
	int store_path_index;
	int filename_len;
	int true_filename_len;
	int crc32;
	int result;
	int len;
	int buff_len;
	bool bSilence;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	nInPackLen = pClientInfo->total_length - sizeof(TrackerHeader);
	pClientInfo->total_length = sizeof(TrackerHeader);
	if (nInPackLen <= FDFS_GROUP_NAME_MAX_LEN)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			INT64_PRINTF_FORMAT" is not correct, " \
			"expect length > %d", __LINE__, \
			STORAGE_PROTO_CMD_QUERY_FILE_INFO, \
			pTask->client_ip,  nInPackLen, \
			FDFS_GROUP_NAME_MAX_LEN);
		return EINVAL;
	}

	if (nInPackLen >= pTask->size)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			INT64_PRINTF_FORMAT" is not correct, " \
			"expect length < %d", __LINE__, \
			STORAGE_PROTO_CMD_QUERY_FILE_INFO, \
			pTask->client_ip, nInPackLen, (int)pTask->size);
		return EINVAL;
	}

	filename_len = nInPackLen - FDFS_GROUP_NAME_MAX_LEN;

	bSilence = ((TrackerHeader *)pTask->data)->status != 0;
	in_buff = pTask->data + sizeof(TrackerHeader);
	memcpy(group_name, in_buff, FDFS_GROUP_NAME_MAX_LEN);
	*(group_name + FDFS_GROUP_NAME_MAX_LEN) = '\0';
	if (strcmp(group_name, g_group_name) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, group_name: %s " \
			"not correct, should be: %s", \
			__LINE__, pTask->client_ip, \
			group_name, g_group_name);
		return EINVAL;
	}

	filename = in_buff + FDFS_GROUP_NAME_MAX_LEN;
	*(filename + filename_len) = '\0';

	true_filename_len = filename_len;
	if ((result=storage_split_filename_ex(filename, &true_filename_len, \
			true_filename, &store_path_index)) != 0)
	{
		return result;
	}
	if ((result=fdfs_check_data_filename(true_filename, \
			true_filename_len)) != 0)
	{
		return result;
	}

	if ((result=trunk_file_lstat(store_path_index, true_filename, \
			true_filename_len, &file_stat, \
			&trunkInfo)) != 0)
	{
		if ((!bSilence) || (result != ENOENT))
		{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, call lstat logic file: %s fail, " \
			"errno: %d, error info: %s", \
			__LINE__, pTask->client_ip, filename, \
			result, STRERROR(result));
		}

		return result;
	}

	if (S_ISLNK(file_stat.st_mode))
	{
		char full_filename[MAX_PATH_SIZE + 128];

		sprintf(full_filename, "%s/data/%s", \
			g_store_paths[store_path_index], true_filename);
		if ((len=readlink(full_filename, src_filename, \
			sizeof(src_filename))) < 0)
		{
			result = errno != 0 ? errno : EPERM;
			logError("file: "__FILE__", line: %d, " \
				"client ip:%s, call readlink file %s fail, " \
				"errno: %d, error info: %s", \
				__LINE__, pTask->client_ip, true_filename, 
				result, STRERROR(result));
			return result;
		}

		*(src_filename + len) = '\0';
		strcpy(full_filename, src_filename);
		if (stat(full_filename, &file_stat) != 0)
		{
			result = errno != 0 ? errno : ENOENT;
			logError("file: "__FILE__", line: %d, " \
				"client ip:%s, call stat file %s fail, " \
				"errno: %d, error info: %s", \
				__LINE__, pTask->client_ip, true_filename, 
				result, STRERROR(result));
			return result;
		}
	}

	if (filename_len < FDFS_LOGIC_FILE_PATH_LEN + \
				FDFS_FILENAME_BASE64_LENGTH)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, length of filename: %s " \
			"is too small, should >= %d", \
			__LINE__, pTask->client_ip, filename, \
			FDFS_LOGIC_FILE_PATH_LEN + FDFS_FILENAME_BASE64_LENGTH);
		return EINVAL;
	}

	memset(decode_buff, 0, sizeof(decode_buff));
	base64_decode_auto(&g_base64_context, filename + \
		FDFS_LOGIC_FILE_PATH_LEN, FDFS_FILENAME_BASE64_LENGTH, \
		decode_buff, &buff_len);
	crc32 = buff2int(decode_buff + sizeof(int) * 4);

	p = pTask->data + sizeof(TrackerHeader);
	long2buff(file_stat.st_size, p);
	p += FDFS_PROTO_PKG_LEN_SIZE;
	long2buff(file_stat.st_mtime, p);
	p += FDFS_PROTO_PKG_LEN_SIZE;
	long2buff(crc32, p);
	p += FDFS_PROTO_PKG_LEN_SIZE;

	pClientInfo->total_length = p - pTask->data;
	return 0;
}

#define CHECK_TRUNK_SERVER(pTask) \
	if (!g_if_trunker_self) \
	{ \
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, i am not trunk server!", \
			__LINE__, pTask->client_ip); \
		return EINVAL; \
	}

/**
request package format:
FDFS_GROUP_NAME_MAX_LEN bytes: group_name
4 bytes: file size

response package format:
1 byte: store_path_index
1 byte: sub_path_high
1 byte: sub_path_low
4 bytes: trunk file id
4 bytes: trunk offset
4 bytes: trunk size
**/
static int storage_server_trunk_alloc_space(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	FDFSTrunkInfoBuff *pApplyBody;
	char *in_buff;
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	FDFSTrunkFullInfo trunkInfo;
	int64_t nInPackLen;
	int file_size;
	int result;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	nInPackLen = pClientInfo->total_length - sizeof(TrackerHeader);
	pClientInfo->total_length = sizeof(TrackerHeader);

	CHECK_TRUNK_SERVER(pTask)

	if (nInPackLen != FDFS_GROUP_NAME_MAX_LEN + 4)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			INT64_PRINTF_FORMAT" is not correct, " \
			"expect length: %d", __LINE__, \
			STORAGE_PROTO_CMD_TRUNK_ALLOC_SPACE, \
			pTask->client_ip,  nInPackLen, \
			FDFS_GROUP_NAME_MAX_LEN + 4);
		return EINVAL;
	}

	in_buff = pTask->data + sizeof(TrackerHeader);
	memcpy(group_name, in_buff, FDFS_GROUP_NAME_MAX_LEN);
	*(group_name + FDFS_GROUP_NAME_MAX_LEN) = '\0';
	if (strcmp(group_name, g_group_name) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, group_name: %s " \
			"not correct, should be: %s", \
			__LINE__, pTask->client_ip, \
			group_name, g_group_name);
		return EINVAL;
	}

	file_size = buff2int(in_buff + FDFS_GROUP_NAME_MAX_LEN);
	if (file_size < 0 || !trunk_check_size(file_size))
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, invalid file size: %d", \
			__LINE__, pTask->client_ip, file_size);
		return EINVAL;
	}

	if ((result=trunk_alloc_space(file_size, &trunkInfo)) != 0)
	{
		return result;
	}

	pApplyBody = (FDFSTrunkInfoBuff *)(pTask->data+sizeof(TrackerHeader));
	pApplyBody->store_path_index = trunkInfo.path.store_path_index;
	pApplyBody->sub_path_high = trunkInfo.path.sub_path_high;
	pApplyBody->sub_path_low = trunkInfo.path.sub_path_low;
	int2buff(trunkInfo.file.id, pApplyBody->id);
	int2buff(trunkInfo.file.offset, pApplyBody->offset);
	int2buff(trunkInfo.file.size, pApplyBody->size);

	pClientInfo->total_length = sizeof(TrackerHeader) + \
				sizeof(FDFSTrunkInfoBuff);
	return 0;
}

#define storage_server_trunk_alloc_confirm(pTask) \
	storage_server_trunk_confirm_or_free(pTask)

#define storage_server_trunk_free_space(pTask) \
	storage_server_trunk_confirm_or_free(pTask)

/**
request package format:
  FDFS_GROUP_NAME_MAX_LEN bytes: group_name
  1 byte: store_path_index
  1 byte: sub_path_high
  1 byte: sub_path_low
  4 bytes: trunk file id
  4 bytes: trunk offset
  4 bytes: trunk size
**/
static int storage_server_trunk_confirm_or_free(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	TrackerHeader *pHeader;
	FDFSTrunkInfoBuff *pTrunkBuff;
	char *in_buff;
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	FDFSTrunkFullInfo trunkInfo;
	int64_t nInPackLen;

	pHeader = (TrackerHeader *)pTask->data;
	pClientInfo = (StorageClientInfo *)pTask->arg;
	nInPackLen = pClientInfo->total_length - sizeof(TrackerHeader);
	pClientInfo->total_length = sizeof(TrackerHeader);

	CHECK_TRUNK_SERVER(pTask)

	if (nInPackLen != STORAGE_TRUNK_ALLOC_CONFIRM_REQ_BODY_LEN)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			INT64_PRINTF_FORMAT" is not correct, " \
			"expect length: %d", __LINE__, \
			pHeader->cmd, pTask->client_ip,  nInPackLen, \
			(int)STORAGE_TRUNK_ALLOC_CONFIRM_REQ_BODY_LEN);
		return EINVAL;
	}

	in_buff = pTask->data + sizeof(TrackerHeader);
	memcpy(group_name, in_buff, FDFS_GROUP_NAME_MAX_LEN);
	*(group_name + FDFS_GROUP_NAME_MAX_LEN) = '\0';
	if (strcmp(group_name, g_group_name) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, group_name: %s " \
			"not correct, should be: %s", \
			__LINE__, pTask->client_ip, \
			group_name, g_group_name);
		return EINVAL;
	}

	pTrunkBuff = (FDFSTrunkInfoBuff *)(in_buff + FDFS_GROUP_NAME_MAX_LEN);
	trunkInfo.path.store_path_index = pTrunkBuff->store_path_index;
	trunkInfo.path.sub_path_high = pTrunkBuff->sub_path_high;
	trunkInfo.path.sub_path_low = pTrunkBuff->sub_path_low;
	trunkInfo.file.id = buff2int(pTrunkBuff->id);
	trunkInfo.file.offset = buff2int(pTrunkBuff->offset);
	trunkInfo.file.size = buff2int(pTrunkBuff->size);

	if (pHeader->cmd == STORAGE_PROTO_CMD_TRUNK_ALLOC_CONFIRM)
	{
		trunkInfo.status = FDFS_TRUNK_STATUS_HOLD;
		return trunk_alloc_confirm(&trunkInfo, pHeader->status);
	}
	else
	{
		trunkInfo.status = FDFS_TRUNK_STATUS_FREE;
		return trunk_free_space(&trunkInfo, true);
	}
}

static int storage_server_fetch_one_path_binlog_dealer( \
		struct fast_task_info *pTask)
{
#define STORAGE_LAST_AHEAD_BYTES   (2 * FDFS_PROTO_PKG_LEN_SIZE)

	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	StorageBinLogReader *pReader;
	char *pOutBuff;
	char *pBasePath;
	int result;
	int record_len;
	int base_path_len;
	int len;
	int store_path_index;
	struct stat stat_buf;
	char diskLogicPath[16];
	char full_filename[MAX_PATH_SIZE];
	char src_filename[MAX_PATH_SIZE];
	bool bLast;
	StorageBinLogRecord record;
	int64_t pkg_len;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	if (pClientInfo->total_length - pClientInfo->total_offset <= \
		STORAGE_LAST_AHEAD_BYTES)  //finished, close the connection
	{
		task_finish_clean_up(pTask);
		return 0;
	}

	pFileContext =  &(pClientInfo->file_context);
	pReader = (StorageBinLogReader *)pClientInfo->extra_arg;

	store_path_index = pFileContext->extra_info.upload.store_path_index;
	pBasePath = g_store_paths[store_path_index];
	base_path_len = strlen(pBasePath);
	pOutBuff = pTask->data;

	bLast = false;
	sprintf(diskLogicPath, "%c"STORAGE_DATA_DIR_FORMAT, \
		FDFS_STORAGE_STORE_PATH_PREFIX_CHAR, store_path_index);

	do
	{
		result = storage_binlog_read(pReader, &record, &record_len);
		if (result == ENOENT)  //no binlog record
		{
			bLast = true;
			result = 0;
			break;
		}
		else if (result != 0)
		{
			break;
		}

		if (g_store_paths[record.store_path_index] != pBasePath)
		{
			continue;
		}

		if (!(record.op_type == STORAGE_OP_TYPE_SOURCE_CREATE_FILE
		   || record.op_type == STORAGE_OP_TYPE_REPLICA_CREATE_FILE
		   || record.op_type == STORAGE_OP_TYPE_SOURCE_CREATE_LINK
		   || record.op_type == STORAGE_OP_TYPE_REPLICA_CREATE_LINK))
		{
			continue;
		}

		snprintf(full_filename, sizeof(full_filename), "%s/data/%s", \
			g_store_paths[record.store_path_index], \
			record.true_filename);
		if (lstat(full_filename, &stat_buf) != 0)
		{
			if (errno == ENOENT)
			{
				continue;
			}
			else
			{
				logError("file: "__FILE__", line: %d, " \
					"call stat fail, file: %s, "\
					"error no: %d, error info: %s", \
					__LINE__, full_filename, \
					errno, STRERROR(errno));
				result = errno != 0 ? errno : EPERM;
				break;
			}
		}

		if (S_ISLNK(stat_buf.st_mode))
		{
			if (record.op_type == STORAGE_OP_TYPE_SOURCE_CREATE_FILE
		 	|| record.op_type == STORAGE_OP_TYPE_REPLICA_CREATE_FILE)
			{
				logWarning("file: "__FILE__", line: %d, " \
					"regular file %s change to symbol " \
					"link file, some mistake happen?", \
					__LINE__, full_filename);
			if (record.op_type == STORAGE_OP_TYPE_SOURCE_CREATE_FILE)
			{
			record.op_type = STORAGE_OP_TYPE_SOURCE_CREATE_LINK;
			}
			else
			{
			record.op_type = STORAGE_OP_TYPE_REPLICA_CREATE_LINK;
			}
			}
		}
		else
		{
			if (record.op_type == STORAGE_OP_TYPE_SOURCE_CREATE_LINK 
		 	|| record.op_type == STORAGE_OP_TYPE_REPLICA_CREATE_LINK)
			{
				logWarning("file: "__FILE__", line: %d, " \
					"symbol link file %s change to " \
					"regular file, some mistake happen?", \
					__LINE__, full_filename);
			if (record.op_type == STORAGE_OP_TYPE_SOURCE_CREATE_LINK)
			{
			record.op_type = STORAGE_OP_TYPE_SOURCE_CREATE_FILE;
			}
			else
			{
			record.op_type = STORAGE_OP_TYPE_REPLICA_CREATE_FILE;
			}
			}
		}

		if (record.op_type == STORAGE_OP_TYPE_SOURCE_CREATE_FILE
		 || record.op_type == STORAGE_OP_TYPE_REPLICA_CREATE_FILE)
		{
			pOutBuff += sprintf(pOutBuff, "%d %c %s\n", \
					(int)record.timestamp, \
					record.op_type, record.filename);
		}
		else
		{
			if ((len=readlink(full_filename, src_filename, \
					sizeof(src_filename))) < 0)
			{
				result = errno != 0 ? errno : EPERM;
				logError("file: "__FILE__", line: %d, " \
					"client ip: %s, call readlink file " \
					"%s fail, errno: %d, error info: %s", \
					__LINE__, pTask->client_ip, \
					full_filename, result, STRERROR(result));

				if (result == ENOENT)
				{
					continue;
				}
				break;
			}

			if (len <= base_path_len)
			{
				logWarning("file: "__FILE__", line: %d, " \
					"invalid symbol link file: %s, " \
					"maybe not create by FastDFS?", \
					__LINE__, full_filename);
				continue;
			}
			*(src_filename + len) = '\0';
			if (!fileExists(src_filename))
			{
				logWarning("file: "__FILE__", line: %d, " \
					"client ip: %s, symbol link file: %s, "\
					"it's source file: %s not exist", \
					__LINE__, pTask->client_ip, \
					full_filename, src_filename);
				continue;
			}

			//full filename format: ${base_path}/data/filename
			pOutBuff += sprintf(pOutBuff, "%d %c %s %s/%s\n", \
					(int)record.timestamp, \
					record.op_type, record.filename, \
					diskLogicPath, \
					src_filename + base_path_len + 6);
		}

		if (pTask->size - (pOutBuff - pTask->data) < \
			STORAGE_BINLOG_LINE_SIZE + FDFS_PROTO_PKG_LEN_SIZE)
		{
			break;
		}
	} while(1);

	if (result != 0) //error occurs
	{
		task_finish_clean_up(pTask);
		return result;
	}

	pTask->length = pOutBuff - pTask->data;
	if (bLast)
	{
		pkg_len = pClientInfo->total_offset + pTask->length - \
				sizeof(TrackerHeader);
		long2buff(pkg_len, pOutBuff);

		pTask->length += FDFS_PROTO_PKG_LEN_SIZE;
		pClientInfo->total_length = pkg_len + FDFS_PROTO_PKG_LEN_SIZE \
						+ STORAGE_LAST_AHEAD_BYTES;
	}

	storage_nio_notify(pTask);
	return 0;
}

static void fetch_one_path_binlog_finish_clean_up(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	StorageBinLogReader *pReader;
	char full_filename[MAX_PATH_SIZE];

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pReader = (StorageBinLogReader *)pClientInfo->extra_arg;
	if (pReader == NULL)
	{
		return;
	}

	pClientInfo->extra_arg = NULL;

	storage_reader_destroy(pReader);
	get_mark_filename_by_reader(pReader, full_filename);
	if (fileExists(full_filename))
	{
		unlink(full_filename);
	}

	free(pReader);
}

static int storage_server_do_fetch_one_path_binlog( \
		struct fast_task_info *pTask, const int store_path_index)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	StorageBinLogReader *pReader;
	TrackerHeader *pHeader;
	int result;

	pReader = (StorageBinLogReader *)malloc(sizeof(StorageBinLogReader));
	if (pReader == NULL)
	{
		logError("file: "__FILE__", line: %d, " \
			"malloc %d bytes fail, errno: %d, error info: %s", \
			__LINE__, (int)sizeof(StorageBinLogReader),
			errno, STRERROR(errno));
		return errno != 0 ? errno : ENOMEM;
	}

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	if ((result=storage_reader_init(NULL, pReader)) != 0)
	{
		storage_reader_destroy(pReader);
		return result;
	}

	pClientInfo->deal_func = storage_server_fetch_one_path_binlog_dealer;
	pClientInfo->clean_func = fetch_one_path_binlog_finish_clean_up;

	pFileContext->fd = -1;
	pFileContext->op = FDFS_STORAGE_FILE_OP_READ;
	pFileContext->dio_thread_index = storage_dio_get_thread_index( \
		pTask, store_path_index, pFileContext->op);
	pFileContext->extra_info.upload.store_path_index = store_path_index;
	pClientInfo->extra_arg = pReader;

	pClientInfo->total_length = INFINITE_FILE_SIZE + \
					sizeof(TrackerHeader);
	pClientInfo->total_offset = 0;
	pTask->length = sizeof(TrackerHeader);
	pHeader = (TrackerHeader *)pTask->data;
	pHeader->status = 0;
	pHeader->cmd = STORAGE_PROTO_CMD_RESP;
	long2buff(pClientInfo->total_length - sizeof(TrackerHeader), \
			pHeader->pkg_len);

	storage_nio_notify(pTask);

	return STORAGE_STATUE_DEAL_FILE;
}

/**
FDFS_GROUP_NAME_MAX_LEN bytes: group_name
1 byte: store path index
**/
static int storage_server_fetch_one_path_binlog(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	char *in_buff;
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	int store_path_index;
	int64_t nInPackLen;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	nInPackLen = pClientInfo->total_length - sizeof(TrackerHeader);
	pClientInfo->total_length = sizeof(TrackerHeader);
	if (nInPackLen != FDFS_GROUP_NAME_MAX_LEN + 1)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			INT64_PRINTF_FORMAT" is not correct, " \
			"expect length = %d", __LINE__, \
			STORAGE_PROTO_CMD_FETCH_ONE_PATH_BINLOG, \
			pTask->client_ip,  \
			nInPackLen, FDFS_GROUP_NAME_MAX_LEN + 1);
		return EINVAL;
	}

	in_buff = pTask->data + sizeof(TrackerHeader);
	memcpy(group_name, in_buff, FDFS_GROUP_NAME_MAX_LEN);
	*(group_name + FDFS_GROUP_NAME_MAX_LEN) = '\0';
	if (strcmp(group_name, g_group_name) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, group_name: %s " \
			"not correct, should be: %s", \
			__LINE__, pTask->client_ip, \
			group_name, g_group_name);
		return EINVAL;
	}

	store_path_index = *(in_buff + FDFS_GROUP_NAME_MAX_LEN);
	if (store_path_index < 0 || store_path_index >= g_path_count)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, store_path_index: %d " \
			"is invalid", __LINE__, \
			pTask->client_ip, store_path_index);
		return EINVAL;
	}

	return storage_server_do_fetch_one_path_binlog( \
			pTask, store_path_index);
}

/**
1 byte: store path index
8 bytes: file size 
FDFS_FILE_EXT_NAME_MAX_LEN bytes: file ext name, do not include dot (.)
file size bytes: file content
**/
static int storage_upload_file(struct fast_task_info *pTask, bool bAppenderFile)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	DisconnectCleanFunc clean_func;
	char *p;
	char filename[128];
	char file_ext_name[FDFS_FILE_PREFIX_MAX_LEN + 1];
	int64_t nInPackLen;
	int64_t file_offset;
	int64_t file_bytes;
	int crc32;
	int store_path_index;
	int result;
	int filename_len;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	nInPackLen = pClientInfo->total_length - sizeof(TrackerHeader);

	if (nInPackLen < 1 + FDFS_PROTO_PKG_LEN_SIZE + 
			FDFS_FILE_EXT_NAME_MAX_LEN)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			INT64_PRINTF_FORMAT" is not correct, " \
			"expect length >= %d", __LINE__, \
			STORAGE_PROTO_CMD_UPLOAD_FILE, \
			pTask->client_ip,  nInPackLen, \
			1 + FDFS_PROTO_PKG_LEN_SIZE + \
			FDFS_FILE_EXT_NAME_MAX_LEN);
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	p = pTask->data + sizeof(TrackerHeader);
	store_path_index = *p++;
	if (store_path_index < 0 || store_path_index >= g_path_count)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, store_path_index: %d " \
			"is invalid", __LINE__, \
			pTask->client_ip, store_path_index);
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	file_bytes = buff2long(p);
	p += FDFS_PROTO_PKG_LEN_SIZE;
	if (file_bytes < 0 || file_bytes != nInPackLen - \
			(1 + FDFS_PROTO_PKG_LEN_SIZE + \
			 FDFS_FILE_EXT_NAME_MAX_LEN))
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, pkg length is not correct, " \
			"invalid file bytes: "INT64_PRINTF_FORMAT \
			", total body length: "INT64_PRINTF_FORMAT, \
			__LINE__, pTask->client_ip, file_bytes, nInPackLen);
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	memcpy(file_ext_name, p, FDFS_FILE_EXT_NAME_MAX_LEN);
	*(file_ext_name + FDFS_FILE_EXT_NAME_MAX_LEN) = '\0';
	p += FDFS_FILE_EXT_NAME_MAX_LEN;

	pFileContext->calc_crc32 = true;
	pFileContext->calc_file_hash = g_check_file_duplicate;
	pFileContext->extra_info.upload.start_time = time(NULL);

	strcpy(pFileContext->extra_info.upload.file_ext_name, file_ext_name);
	storage_format_ext_name(file_ext_name, \
			pFileContext->extra_info.upload.formatted_ext_name);

	pFileContext->sync_flag = STORAGE_OP_TYPE_SOURCE_CREATE_FILE;
	pFileContext->timestamp2log = pFileContext->extra_info.upload.start_time;
	pFileContext->extra_info.upload.if_appender_file = bAppenderFile;
	pFileContext->op = FDFS_STORAGE_FILE_OP_WRITE;
	if (bAppenderFile)
	{
		pFileContext->extra_info.upload.if_trunk_file = false;
	}
	else
	{
		pFileContext->extra_info.upload.if_trunk_file = \
			g_if_use_trunk_file && trunk_check_size( \
			TRUNK_CALC_SIZE(file_bytes));
	}

	if (pFileContext->extra_info.upload.if_trunk_file)
	{
		FDFSTrunkFullInfo *pTrunkInfo;

		pTrunkInfo = &(pFileContext->extra_info.upload.trunk_info);
		if ((result=trunk_client_trunk_alloc_space( \
			TRUNK_CALC_SIZE(file_bytes), pTrunkInfo)) != 0)
		{
			pClientInfo->total_length = sizeof(TrackerHeader);
			return result;
		}

		clean_func = dio_trunk_write_finish_clean_up;
		file_offset = TRUNK_FILE_START_OFFSET((*pTrunkInfo));
        	pFileContext->extra_info.upload.if_gen_filename = true;
		trunk_get_full_filename(pTrunkInfo, pFileContext->filename, \
				sizeof(pFileContext->filename));
		pFileContext->extra_info.upload.store_path_index = \
				pTrunkInfo->path.store_path_index;
		pFileContext->extra_info.upload.before_open_callback = \
					dio_check_trunk_file;
		pFileContext->extra_info.upload.before_close_callback = \
					dio_write_chunk_header;
		pFileContext->open_flags = O_WRONLY | extra_open_flags;
	}
	else
	{
		crc32 = rand();
		*filename = '\0';
		filename_len = 0;
		if ((result=storage_get_filename(pClientInfo, \
			pFileContext->extra_info.upload.start_time, \
			store_path_index, file_bytes, crc32, \
			pFileContext->extra_info.upload.formatted_ext_name, \
			filename, &filename_len, pFileContext->filename)) != 0)
		{
			pClientInfo->total_length = sizeof(TrackerHeader);
			return result;
		}

		clean_func = dio_write_finish_clean_up;
		file_offset = 0;
        	pFileContext->extra_info.upload.if_gen_filename = true;
		pFileContext->extra_info.upload.store_path_index = store_path_index;
		pFileContext->extra_info.upload.before_open_callback = NULL;
		pFileContext->extra_info.upload.before_close_callback = NULL;
		pFileContext->open_flags = O_WRONLY | O_CREAT | O_TRUNC \
						| extra_open_flags;
	}

 	return storage_write_to_file(pTask, file_offset, file_bytes, \
			p - pTask->data, dio_write_file, \
			storage_upload_file_done_callback, \
			clean_func, store_path_index);
}

static int storage_deal_active_test(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	int64_t nInPackLen;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	nInPackLen = pClientInfo->total_length - sizeof(TrackerHeader);
	pClientInfo->total_length = sizeof(TrackerHeader);
	if (nInPackLen != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			INT64_PRINTF_FORMAT" is not correct, " \
			"expect length 0", __LINE__, \
			FDFS_PROTO_CMD_ACTIVE_TEST, pTask->client_ip, \
			nInPackLen);
		return EINVAL;
	}

	return 0;
}

/**
8 bytes: appender filename length
8 bytes: file size
appender filename bytes: appender filename
file size bytes: file content
**/
static int storage_append_file(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	char *p;
	char appender_filename[128];
	char true_filename[128];
	char buff[64];
	struct stat stat_buf;
	int appender_filename_len;
	int64_t nInPackLen;
	int64_t file_bytes;
	int64_t appender_file_size;
	int result;
	int store_path_index;
	int filename_len;
	int buff_len;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	nInPackLen = pClientInfo->total_length - sizeof(TrackerHeader);

	if (nInPackLen <= 2 * FDFS_PROTO_PKG_LEN_SIZE)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			INT64_PRINTF_FORMAT" is not correct, " \
			"expect length > %d", __LINE__, \
			STORAGE_PROTO_CMD_APPEND_FILE, \
			pTask->client_ip,  \
			nInPackLen, 2 * FDFS_PROTO_PKG_LEN_SIZE);
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	p = pTask->data + sizeof(TrackerHeader);

	appender_filename_len = buff2long(p);
	p += FDFS_PROTO_PKG_LEN_SIZE;
	file_bytes = buff2long(p);
	p += FDFS_PROTO_PKG_LEN_SIZE;
	if (appender_filename_len < FDFS_LOGIC_FILE_PATH_LEN + \
		FDFS_FILENAME_BASE64_LENGTH + FDFS_FILE_EXT_NAME_MAX_LEN + 1 \
		|| appender_filename_len >= sizeof(appender_filename))
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, invalid appender_filename " \
			"bytes: %d", __LINE__, \
			pTask->client_ip, appender_filename_len);
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	if (file_bytes < 0 || file_bytes != nInPackLen - \
		(2 * FDFS_PROTO_PKG_LEN_SIZE + appender_filename_len))
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, pkg length is not correct, " \
			"invalid file bytes: "INT64_PRINTF_FORMAT, \
			__LINE__, pTask->client_ip, file_bytes);
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	memcpy(appender_filename, p, appender_filename_len);
	*(appender_filename + appender_filename_len) = '\0';
	p += appender_filename_len;

	filename_len = appender_filename_len;
	if ((result=storage_split_filename_ex(appender_filename, \
		&filename_len, true_filename, &store_path_index)) != 0)
	{
		pClientInfo->total_length = sizeof(TrackerHeader);
		return result;
	}
	if ((result=fdfs_check_data_filename(true_filename, filename_len)) != 0)
	{
		pClientInfo->total_length = sizeof(TrackerHeader);
		return result;
	}

	snprintf(pFileContext->filename, sizeof(pFileContext->filename), \
		"%s/data/%s", g_store_paths[store_path_index], true_filename);
	if (lstat(pFileContext->filename, &stat_buf) == 0)
	{
		if (!S_ISREG(stat_buf.st_mode))
		{
			logError("file: "__FILE__", line: %d, " \
				"client ip: %s, appender file: %s " \
				"is not a regular file", __LINE__, \
				pTask->client_ip, pFileContext->filename);

			pClientInfo->total_length = sizeof(TrackerHeader);
			return EINVAL;
		}
	}
	else
	{
		pClientInfo->total_length = sizeof(TrackerHeader);
		result = errno != 0 ? errno : ENOENT;
		if (result == ENOENT)
		{
			logError("file: "__FILE__", line: %d, " \
				"client ip: %s, appender file: %s " \
				"not exist", __LINE__, \
				pTask->client_ip, pFileContext->filename);
		}
		else
		{
			logError("file: "__FILE__", line: %d, " \
				"client ip: %s, stat appednder file %s fail" \
				", errno: %d, error info: %s.", \
				__LINE__, pTask->client_ip, \
				pFileContext->filename, \
				result, STRERROR(result));
		}

		return result;
	}

	strcpy(pFileContext->fname2log, appender_filename);

	memset(buff, 0, sizeof(buff));
	base64_decode_auto(&g_base64_context, pFileContext->fname2log + \
		FDFS_LOGIC_FILE_PATH_LEN, FDFS_FILENAME_BASE64_LENGTH, \
		buff, &buff_len);

	appender_file_size = buff2long(buff + sizeof(int) * 2);
	if (appender_file_size != FDFS_APPENDER_FILE_SIZE)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, file: %s is not a valid " \
			"appender file, file size: "INT64_PRINTF_FORMAT, \
			__LINE__, pTask->client_ip, appender_filename, \
			appender_file_size);

		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	if (file_bytes == 0)
	{
		pClientInfo->total_length = sizeof(TrackerHeader);
		return 0;
	}

	pFileContext->extra_info.upload.start_time = time(NULL);
	pFileContext->extra_info.upload.if_gen_filename = false;

	pFileContext->calc_crc32 = false;
	pFileContext->calc_file_hash = false;

	snprintf(pFileContext->filename, sizeof(pFileContext->filename), \
		"%s/data/%s", g_store_paths[store_path_index], true_filename);

	pFileContext->sync_flag = STORAGE_OP_TYPE_SOURCE_APPEND_FILE;
	pFileContext->timestamp2log = pFileContext->extra_info.upload.start_time;
	pFileContext->extra_info.upload.if_appender_file = true;
	pFileContext->extra_info.upload.if_trunk_file = false;
	pFileContext->extra_info.upload.before_open_callback = NULL;
	pFileContext->extra_info.upload.before_close_callback = NULL;
	pFileContext->extra_info.upload.store_path_index = store_path_index;
	pFileContext->op = FDFS_STORAGE_FILE_OP_APPEND;
	pFileContext->open_flags = O_WRONLY | O_APPEND | extra_open_flags;

 	return storage_write_to_file(pTask, stat_buf.st_size, file_bytes, \
			p - pTask->data, dio_write_file, \
			storage_append_file_done_callback, \
			dio_append_finish_clean_up, store_path_index);
}

/**
8 bytes: master filename length
8 bytes: file size
FDFS_FILE_PREFIX_MAX_LEN bytes  : filename prefix
FDFS_FILE_EXT_NAME_MAX_LEN bytes: file ext name, do not include dot (.)
master filename bytes: master filename
file size bytes: file content
**/
static int storage_upload_slave_file(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	char *p;
	char filename[128];
	char master_filename[128];
	char true_filename[128];
	char prefix_name[FDFS_FILE_PREFIX_MAX_LEN + 1];
	char file_ext_name[FDFS_FILE_PREFIX_MAX_LEN + 1];
	int master_filename_len;
	int64_t nInPackLen;
	int64_t file_bytes;
	int crc32;
	int result;
	int store_path_index;
	int filename_len;
	struct stat stat_buf;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	nInPackLen = pClientInfo->total_length - sizeof(TrackerHeader);

	if (nInPackLen <= 2 * FDFS_PROTO_PKG_LEN_SIZE + \
			FDFS_FILE_PREFIX_MAX_LEN + FDFS_FILE_EXT_NAME_MAX_LEN)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			INT64_PRINTF_FORMAT" is not correct, " \
			"expect length > %d", __LINE__, \
			STORAGE_PROTO_CMD_UPLOAD_FILE, \
			pTask->client_ip,  \
			nInPackLen, 2 * FDFS_PROTO_PKG_LEN_SIZE + \
			FDFS_FILE_PREFIX_MAX_LEN + \
			FDFS_FILE_EXT_NAME_MAX_LEN);
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	p = pTask->data + sizeof(TrackerHeader);

	master_filename_len = buff2long(p);
	p += FDFS_PROTO_PKG_LEN_SIZE;
	file_bytes = buff2long(p);
	p += FDFS_PROTO_PKG_LEN_SIZE;
	if (master_filename_len <= FDFS_LOGIC_FILE_PATH_LEN || \
		master_filename_len >= sizeof(master_filename))
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, invalid master_filename " \
			"bytes: %d", __LINE__, \
			pTask->client_ip, master_filename_len);
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	if (file_bytes < 0 || file_bytes != nInPackLen - \
		(2 * FDFS_PROTO_PKG_LEN_SIZE + FDFS_FILE_PREFIX_MAX_LEN
		 + FDFS_FILE_EXT_NAME_MAX_LEN + master_filename_len))
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, pkg length is not correct, " \
			"invalid file bytes: "INT64_PRINTF_FORMAT, \
			__LINE__, pTask->client_ip, file_bytes);
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	memcpy(prefix_name, p, FDFS_FILE_PREFIX_MAX_LEN);
	*(prefix_name + FDFS_FILE_PREFIX_MAX_LEN) = '\0';
	p += FDFS_FILE_PREFIX_MAX_LEN;

	memcpy(file_ext_name, p, FDFS_FILE_EXT_NAME_MAX_LEN);
	*(file_ext_name + FDFS_FILE_EXT_NAME_MAX_LEN) = '\0';
	p += FDFS_FILE_EXT_NAME_MAX_LEN;

	memcpy(master_filename, p, master_filename_len);
	*(master_filename + master_filename_len) = '\0';
	p += master_filename_len;

	filename_len = master_filename_len;
	if ((result=storage_split_filename_ex(master_filename, \
		&filename_len, true_filename, &store_path_index)) != 0)
	{
		pClientInfo->total_length = sizeof(TrackerHeader);
		return result;
	}
	if ((result=fdfs_check_data_filename(true_filename, filename_len)) != 0)
	{
		pClientInfo->total_length = sizeof(TrackerHeader);
		return result;
	}

	if ((result=trunk_file_lstat(store_path_index, true_filename, \
			filename_len, &stat_buf, \
			&(pFileContext->extra_info.upload.trunk_info))) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, stat logic file %s fail, " \
			"errno: %d, error info: %s.", \
			__LINE__, pTask->client_ip, \
			master_filename, result, STRERROR(result));
		return result;
	}

	strcpy(pFileContext->extra_info.upload.file_ext_name, file_ext_name);
	storage_format_ext_name(file_ext_name, \
			pFileContext->extra_info.upload.formatted_ext_name);
	pFileContext->extra_info.upload.start_time = time(NULL);
	pFileContext->extra_info.upload.if_gen_filename = g_check_file_duplicate;
	if (pFileContext->extra_info.upload.if_gen_filename)
	{
	crc32 = rand();
	*filename = '\0';
	filename_len = 0;
	if ((result=storage_get_filename(pClientInfo, \
			pFileContext->extra_info.upload.start_time, \
			store_path_index, file_bytes, crc32, \
			pFileContext->extra_info.upload.formatted_ext_name, \
			filename, &filename_len, pFileContext->filename)) != 0)
	{
		pClientInfo->total_length = sizeof(TrackerHeader);
		return result;
	}

	if (*pFileContext->filename == '\0')
	{
		logWarning("file: "__FILE__", line: %d, " \
			"Can't generate uniq filename", __LINE__);
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EBUSY;
	}
	}
	else
	{
	if ((result=fdfs_gen_slave_filename(true_filename, \
		prefix_name, file_ext_name, filename, &filename_len)) != 0)
	{
		pClientInfo->total_length = sizeof(TrackerHeader);
		return result;
	}
	snprintf(pFileContext->filename, sizeof(pFileContext->filename), \
		"%s/data/%s", g_store_paths[store_path_index], filename);
	if (fileExists(pFileContext->filename))
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, slave file: %s " \
			"already exist", __LINE__, \
			pTask->client_ip, pFileContext->filename);
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EEXIST;
	}
	}

	sprintf(pFileContext->fname2log, "%c"STORAGE_DATA_DIR_FORMAT"/%s", \
			FDFS_STORAGE_STORE_PATH_PREFIX_CHAR, \
			store_path_index, filename);

	pFileContext->calc_crc32 = true;
	pFileContext->calc_file_hash = g_check_file_duplicate;

	strcpy(pFileContext->extra_info.upload.master_filename, master_filename);
	strcpy(pFileContext->extra_info.upload.prefix_name, prefix_name);


	pFileContext->sync_flag = STORAGE_OP_TYPE_SOURCE_CREATE_FILE;
	pFileContext->timestamp2log = pFileContext->extra_info.upload.start_time;
	pFileContext->extra_info.upload.if_appender_file = false;
	pFileContext->extra_info.upload.if_trunk_file = false;
	pFileContext->extra_info.upload.before_open_callback = NULL;
	pFileContext->extra_info.upload.before_close_callback = NULL;
	pFileContext->extra_info.upload.store_path_index = store_path_index;
	pFileContext->op = FDFS_STORAGE_FILE_OP_WRITE;
	pFileContext->open_flags = O_WRONLY | O_CREAT | O_TRUNC | extra_open_flags;

 	return storage_write_to_file(pTask, 0, file_bytes, p - pTask->data, \
			dio_write_file, storage_upload_file_done_callback, \
			dio_write_finish_clean_up, store_path_index);
}

/**
8 bytes: filename bytes
8 bytes: file size
4 bytes: source op timestamp
FDFS_GROUP_NAME_MAX_LEN bytes: group_name
filename bytes : filename
file size bytes: file content
**/
static int storage_sync_copy_file(struct fast_task_info *pTask, \
		const char proto_cmd)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	TaskDealFunc deal_func;
	DisconnectCleanFunc clean_func;
	FDFSTrunkFullInfo trunkInfo;
	char *p;
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	char true_filename[128];
	char filename[128];
	int filename_len;
	int64_t nInPackLen;
	int64_t file_bytes;
	int64_t file_offset;
	int result;
	int store_path_index;
	bool have_file_content;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	nInPackLen = pClientInfo->total_length - sizeof(TrackerHeader);

	if (nInPackLen <= 2 * FDFS_PROTO_PKG_LEN_SIZE + \
		4 + FDFS_GROUP_NAME_MAX_LEN)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			INT64_PRINTF_FORMAT"is not correct, " \
			"expect length > %d", __LINE__, \
			proto_cmd, pTask->client_ip, nInPackLen, \
			2 * FDFS_PROTO_PKG_LEN_SIZE + 4+FDFS_GROUP_NAME_MAX_LEN);
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	p = pTask->data + sizeof(TrackerHeader);

	filename_len = buff2long(p);
	p += FDFS_PROTO_PKG_LEN_SIZE;
	file_bytes = buff2long(p);
	p += FDFS_PROTO_PKG_LEN_SIZE;
	if (filename_len < 0 || filename_len >= sizeof(filename))
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, in request pkg, " \
			"filename length: %d is invalid, " \
			"which < 0 or >= %d", __LINE__, pTask->client_ip, \
			filename_len,  (int)sizeof(filename));
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	if (file_bytes < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, in request pkg, " \
			"file size: "INT64_PRINTF_FORMAT" is invalid, "\
			"which < 0", __LINE__, pTask->client_ip, file_bytes);
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	pFileContext->timestamp2log = buff2int(p);
	p += 4;

	memcpy(group_name, p, FDFS_GROUP_NAME_MAX_LEN);
	*(group_name + FDFS_GROUP_NAME_MAX_LEN) = '\0';
	p += FDFS_GROUP_NAME_MAX_LEN;
	if (strcmp(group_name, g_group_name) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, group_name: %s " \
			"not correct, should be: %s", __LINE__, \
			pTask->client_ip, group_name, g_group_name);
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	have_file_content = ((TrackerHeader *)pTask->data)->status == 0;
	if (have_file_content)
	{
	if (file_bytes != nInPackLen - (2*FDFS_PROTO_PKG_LEN_SIZE + \
			4 + FDFS_GROUP_NAME_MAX_LEN + filename_len))
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, in request pkg, " \
			"file size: "INT64_PRINTF_FORMAT \
			" != remain bytes: "INT64_PRINTF_FORMAT"", \
			__LINE__, pTask->client_ip, file_bytes, \
			nInPackLen - (2*FDFS_PROTO_PKG_LEN_SIZE + \
			FDFS_GROUP_NAME_MAX_LEN + filename_len));
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}
	}
	else
	{
	if (0 != nInPackLen - (2*FDFS_PROTO_PKG_LEN_SIZE + \
			4 + FDFS_GROUP_NAME_MAX_LEN + filename_len))
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, in request pkg, " \
			" remain bytes: "INT64_PRINTF_FORMAT" != 0 ", \
			__LINE__, pTask->client_ip, \
			nInPackLen - (2*FDFS_PROTO_PKG_LEN_SIZE + \
			FDFS_GROUP_NAME_MAX_LEN + filename_len));
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}
	}

	memcpy(filename, p, filename_len);
	*(filename + filename_len) = '\0';
	p += filename_len;

	if ((result=storage_split_filename_ex(filename, \
		&filename_len, true_filename, &store_path_index)) != 0)
	{
		pClientInfo->total_length = sizeof(TrackerHeader);
		return result;
	}
	if ((result=fdfs_check_data_filename(true_filename, filename_len)) != 0)
	{
		pClientInfo->total_length = sizeof(TrackerHeader);
		return result;
	}

	if (proto_cmd == STORAGE_PROTO_CMD_SYNC_CREATE_FILE)
	{
		pFileContext->sync_flag = STORAGE_OP_TYPE_REPLICA_CREATE_FILE;
	}
	else
	{
		pFileContext->sync_flag = STORAGE_OP_TYPE_REPLICA_UPDATE_FILE;
	}

	if (have_file_content)
	{
		pFileContext->op = FDFS_STORAGE_FILE_OP_WRITE;
	}
	else
	{
		pFileContext->op = FDFS_STORAGE_FILE_OP_DISCARD;
		*(pFileContext->filename) = '\0';
	}

	pFileContext->extra_info.upload.if_trunk_file = false;
	if (proto_cmd == STORAGE_PROTO_CMD_SYNC_CREATE_FILE && \
		have_file_content)
	{
		struct stat stat_buf;

		if ((result=trunk_file_lstat(store_path_index, \
			true_filename, filename_len, &stat_buf, \
			&trunkInfo)) != 0)
		{
			if (result != ENOENT)  //accept no exist
			{
			logError("file: "__FILE__", line: %d, " \
				"client ip: %s, stat logic file %s fail, " \
				"errno: %d, error info: %s.", \
				__LINE__, pTask->client_ip, \
				filename, result, STRERROR(result));
			return result;
			}
		}
		else if (!S_ISREG(stat_buf.st_mode))
		{
			logWarning("file: "__FILE__", line: %d, " \
				"client ip: %s, logic file %s is not " \
				"a regular file, will be overwrited", \
				__LINE__, pTask->client_ip, filename);
		}
		else if (stat_buf.st_size != file_bytes)
		{
			logWarning("file: "__FILE__", line: %d, " \
				"client ip: %s, logic file %s, " \
				"my file size: "OFF_PRINTF_FORMAT \
				" != src file size: "INT64_PRINTF_FORMAT \
				", will be overwrited", __LINE__, \
				pTask->client_ip, filename, \
				stat_buf.st_size, file_bytes);
		}
		else
		{
			logWarning("file: "__FILE__", line: %d, " \
				"cmd=%d, client ip: %s, data file: %s " \
				"already exists, ignore it", \
				__LINE__, proto_cmd, \
				pTask->client_ip, filename);

			pFileContext->op = FDFS_STORAGE_FILE_OP_DISCARD;
			*(pFileContext->filename) = '\0';
		}

		pFileContext->extra_info.upload.if_trunk_file = \
				STORAGE_IS_TRUNK_FILE(trunkInfo);
	}
	else
	{
		memset(&trunkInfo, 0, sizeof(trunkInfo));
	}

	if (pFileContext->op == FDFS_STORAGE_FILE_OP_WRITE)
	{
	if (pFileContext->extra_info.upload.if_trunk_file)
	{
		clean_func = dio_trunk_write_finish_clean_up;
		file_offset = TRUNK_FILE_START_OFFSET(trunkInfo);
		trunk_get_full_filename(&trunkInfo, pFileContext->filename, \
				sizeof(pFileContext->filename));
		pFileContext->extra_info.upload.before_open_callback = \
					dio_check_trunk_file;
		pFileContext->extra_info.upload.before_close_callback = \
					dio_write_chunk_header;
		pFileContext->open_flags = O_WRONLY | extra_open_flags;
	}
	else
	{
		#define MKTEMP_MAX_COUNT  10
		int i;
		for (i=0; i < MKTEMP_MAX_COUNT; i++)
		{
			pthread_mutex_lock(&g_storage_thread_lock);

			sprintf(pFileContext->filename, "%s/data/.cp" \
				INT64_PRINTF_FORMAT".tmp", \
				g_store_paths[store_path_index], \
				temp_file_sequence++);

			pthread_mutex_unlock(&g_storage_thread_lock);

			if (fileExists(pFileContext->filename))
			{
				logWarning("file: "__FILE__", line: %d, " \
					"client ip: %s, temp file %s already "\
					"exists", __LINE__, pTask->client_ip, \
					pFileContext->filename);
				continue;
			}

			break;
		}

		if (i == MKTEMP_MAX_COUNT)
		{
			logError("file: "__FILE__", line: %d, " \
				"client ip: %s, make temp file fail", \
				__LINE__, pTask->client_ip);
			pClientInfo->total_length = sizeof(TrackerHeader);
			return EAGAIN;
		}


		clean_func = dio_write_finish_clean_up;
		file_offset = 0;
		pFileContext->extra_info.upload.before_open_callback = NULL;
		pFileContext->extra_info.upload.before_close_callback = NULL;
		pFileContext->open_flags = O_WRONLY | O_CREAT | O_TRUNC \
						| extra_open_flags;
	}

		deal_func = dio_write_file;
	}
	else
	{
		file_offset = 0;
		deal_func = dio_discard_file;
		clean_func = NULL;

		pFileContext->extra_info.upload.before_open_callback = NULL;
		pFileContext->extra_info.upload.before_close_callback = NULL;
	}
	
	pFileContext->calc_crc32 = false;
	pFileContext->calc_file_hash = false;
	strcpy(pFileContext->fname2log, filename);

	if (have_file_content)
	{
		return storage_write_to_file(pTask, file_offset, file_bytes, \
			p - pTask->data, deal_func, \
			storage_sync_copy_file_done_callback, \
			clean_func, store_path_index);
	}
	else
	{
		storage_sync_copy_file_done_callback(pTask, 0);
		return STORAGE_STATUE_DEAL_FILE;
	}
}

/**
8 bytes: filename bytes
8 bytes: start offset
8 bytes: append bytes 
4 bytes: source op timestamp
FDFS_GROUP_NAME_MAX_LEN bytes: group_name
filename bytes : filename
file size bytes: file content
**/
static int storage_sync_append_file(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	TaskDealFunc deal_func;
	char *p;
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	char true_filename[128];
	char filename[128];
	bool need_write_file;
	int filename_len;
	int64_t nInPackLen;
	int64_t start_offset;
	int64_t append_bytes;
	struct stat stat_buf;
	int result;
	int store_path_index;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	nInPackLen = pClientInfo->total_length - sizeof(TrackerHeader);

	if (nInPackLen <= 3 * FDFS_PROTO_PKG_LEN_SIZE + \
		4 + FDFS_GROUP_NAME_MAX_LEN)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			INT64_PRINTF_FORMAT"is not correct, " \
			"expect length > %d", __LINE__, \
			STORAGE_PROTO_CMD_SYNC_APPEND_FILE, \
			pTask->client_ip, nInPackLen, \
			3 * FDFS_PROTO_PKG_LEN_SIZE + 4+FDFS_GROUP_NAME_MAX_LEN);
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	p = pTask->data + sizeof(TrackerHeader);

	filename_len = buff2long(p);
	p += FDFS_PROTO_PKG_LEN_SIZE;
	start_offset = buff2long(p);
	p += FDFS_PROTO_PKG_LEN_SIZE;
	append_bytes = buff2long(p);
	p += FDFS_PROTO_PKG_LEN_SIZE;
	if (filename_len < 0 || filename_len >= sizeof(filename))
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, in request pkg, " \
			"filename length: %d is invalid, " \
			"which < 0 or >= %d", __LINE__, pTask->client_ip, \
			filename_len,  (int)sizeof(filename));
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	if (start_offset < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, in request pkg, " \
			"start offset: "INT64_PRINTF_FORMAT" is invalid, "\
			"which < 0", __LINE__, pTask->client_ip, start_offset);
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	if (append_bytes < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, in request pkg, " \
			"append bytes: "INT64_PRINTF_FORMAT" is invalid, "\
			"which < 0", __LINE__, pTask->client_ip, append_bytes);
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	pFileContext->timestamp2log = buff2int(p);
	p += 4;

	memcpy(group_name, p, FDFS_GROUP_NAME_MAX_LEN);
	*(group_name + FDFS_GROUP_NAME_MAX_LEN) = '\0';
	p += FDFS_GROUP_NAME_MAX_LEN;
	if (strcmp(group_name, g_group_name) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, group_name: %s " \
			"not correct, should be: %s", __LINE__, \
			pTask->client_ip, group_name, g_group_name);
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	if (append_bytes != nInPackLen - (3 * FDFS_PROTO_PKG_LEN_SIZE + \
			4 + FDFS_GROUP_NAME_MAX_LEN + filename_len))
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, in request pkg, " \
			"file size: "INT64_PRINTF_FORMAT \
			" != remain bytes: "INT64_PRINTF_FORMAT"", \
			__LINE__, pTask->client_ip, append_bytes, \
			nInPackLen - (3 * FDFS_PROTO_PKG_LEN_SIZE + \
			FDFS_GROUP_NAME_MAX_LEN + filename_len));
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	memcpy(filename, p, filename_len);
	*(filename + filename_len) = '\0';
	p += filename_len;

	if ((result=storage_split_filename_ex(filename, \
		&filename_len, true_filename, &store_path_index)) != 0)
	{
		pClientInfo->total_length = sizeof(TrackerHeader);
		return result;
	}
	if ((result=fdfs_check_data_filename(true_filename, filename_len)) != 0)
	{
		pClientInfo->total_length = sizeof(TrackerHeader);
		return result;
	}

	snprintf(pFileContext->filename, sizeof(pFileContext->filename), \
			"%s/data/%s", g_store_paths[store_path_index], \
			true_filename);

	if (lstat(pFileContext->filename, &stat_buf) != 0)
	{
		result = errno != 0 ? errno : ENOENT;
		if (result != ENOENT)
		{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, stat file %s fail, " \
			"errno: %d, error info: %s.", \
			__LINE__, pTask->client_ip, \
			pFileContext->filename, result, STRERROR(result));
		return result;
		}
		else
		{
		logWarning("file: "__FILE__", line: %d, " \
			"client ip: %s, appender file %s not exists, " \
			"will be resynced", __LINE__, pTask->client_ip, \
			pFileContext->filename);
		need_write_file = false;
		}
	}
	else if (!S_ISREG(stat_buf.st_mode))
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, file %s is not a regular " \
			"file, will be ignored",  __LINE__, \
			pTask->client_ip, pFileContext->filename);
		need_write_file = false;
	}
	else if (stat_buf.st_size == start_offset)
	{
		need_write_file = true;
	}
	else if (stat_buf.st_size > start_offset)
	{
		if (stat_buf.st_size >= start_offset + append_bytes)
		{
		logDebug("file: "__FILE__", line: %d, " \
			"client ip: %s, file %s,  my file size: " \
			OFF_PRINTF_FORMAT" >= src file size: " \
			INT64_PRINTF_FORMAT", do not append", \
			__LINE__, pTask->client_ip, pFileContext->filename, \
			stat_buf.st_size, start_offset + append_bytes);
		}
		else
		{
		logWarning("file: "__FILE__", line: %d, " \
			"client ip: %s, file %s,  my file size: " \
			OFF_PRINTF_FORMAT" > "INT64_PRINTF_FORMAT \
			", but < "INT64_PRINTF_FORMAT", need be resynced", \
			__LINE__, pTask->client_ip, pFileContext->filename, \
			stat_buf.st_size, start_offset, \
			start_offset + append_bytes);

		}

		need_write_file = false;
	}
	else //stat_buf.st_size < start_offset
	{
		logWarning("file: "__FILE__", line: %d, " \
			"client ip: %s, file %s,  my file size: " \
			OFF_PRINTF_FORMAT" < start offset " \
			INT64_PRINTF_FORMAT", need to resync this file!", \
			__LINE__, pTask->client_ip, pFileContext->filename, \
			stat_buf.st_size, start_offset);
		need_write_file = false;
	}

	pFileContext->sync_flag = STORAGE_OP_TYPE_REPLICA_APPEND_FILE;

	if (need_write_file)
	{
		deal_func = dio_write_file;
		pFileContext->op = FDFS_STORAGE_FILE_OP_APPEND;
		pFileContext->open_flags = O_WRONLY | O_APPEND | extra_open_flags;

		snprintf(pFileContext->fname2log, \
			sizeof(pFileContext->fname2log), \
			"%s "INT64_PRINTF_FORMAT" "INT64_PRINTF_FORMAT, \
			filename, start_offset, append_bytes);
	}
	else
	{
		deal_func = dio_discard_file;
		pFileContext->op = FDFS_STORAGE_FILE_OP_DISCARD;
		pFileContext->open_flags = 0;
	}

	pFileContext->calc_crc32 = false;
	pFileContext->calc_file_hash = false;
	pFileContext->extra_info.upload.before_open_callback = NULL;
	pFileContext->extra_info.upload.before_close_callback = NULL;

	return storage_write_to_file(pTask, start_offset, append_bytes, \
		p - pTask->data, deal_func, \
		storage_sync_append_file_done_callback, \
		dio_append_finish_clean_up, store_path_index);
}

/**
8 bytes: dest(link) filename length
8 bytes: source filename length
4 bytes: source op timestamp
FDFS_GROUP_NAME_MAX_LEN bytes: group_name
dest filename length: dest filename
source filename length: source filename
**/
static int storage_do_sync_link_file(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	TrackerHeader *pHeader;
	char *p;
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	char src_filename[128];
	char src_true_filename[128];
	char src_full_filename[MAX_PATH_SIZE];
	int64_t nInPackLen;
	int dest_filename_len;
	int src_filename_len;
	int result;
	int src_store_path_index;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	nInPackLen = pClientInfo->total_length - sizeof(TrackerHeader);
	pClientInfo->total_length = sizeof(TrackerHeader);

	do
	{
	p = pTask->data + sizeof(TrackerHeader);

	dest_filename_len = buff2long(p);
	p += FDFS_PROTO_PKG_LEN_SIZE;

	src_filename_len = buff2long(p);
	p += FDFS_PROTO_PKG_LEN_SIZE + 4;

	if (src_filename_len < 0 || src_filename_len >= sizeof(src_filename))
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, in request pkg, " \
			"filename length: %d is invalid, " \
			"which < 0 or >= %d", \
			__LINE__, pTask->client_ip, \
			src_filename_len, (int)sizeof(src_filename));
		result = EINVAL;
		break;
	}

	memcpy(group_name, p, FDFS_GROUP_NAME_MAX_LEN);
	*(group_name + FDFS_GROUP_NAME_MAX_LEN) = '\0';
	p += FDFS_GROUP_NAME_MAX_LEN;
	if (strcmp(group_name, g_group_name) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, group_name: %s " \
			"not correct, should be: %s", \
			__LINE__, pTask->client_ip, \
			group_name, g_group_name);
		result = EINVAL;
		break;
	}

	if (nInPackLen != 2 * FDFS_PROTO_PKG_LEN_SIZE + 4 + \
		FDFS_GROUP_NAME_MAX_LEN + dest_filename_len + src_filename_len)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, in request pkg, " \
			"pgk length: "INT64_PRINTF_FORMAT \
			" != bytes: %d", __LINE__, pTask->client_ip, \
			nInPackLen, 2 * FDFS_PROTO_PKG_LEN_SIZE + \
			FDFS_GROUP_NAME_MAX_LEN + dest_filename_len + \
			src_filename_len);
		result = EINVAL;
		break;
	}

	p += dest_filename_len;

	memcpy(src_filename, p, src_filename_len);
	*(src_filename + src_filename_len) = '\0';
	p += src_filename_len;

	if ((result=storage_split_filename_ex(src_filename, &src_filename_len,
			 src_true_filename, &src_store_path_index)) != 0)
	{
		break;
	}
	if ((result=fdfs_check_data_filename(src_true_filename, \
			src_filename_len)) != 0)
	{
		break;
	}
	snprintf(src_full_filename, sizeof(src_full_filename), \
			"%s/data/%s", g_store_paths[src_store_path_index], \
			src_true_filename);

	if (fileExists(pFileContext->filename))
	{
		logWarning("file: "__FILE__", line: %d, " \
			"client ip: %s, data file: %s " \
			"already exists, ignore it", __LINE__, \
			pTask->client_ip, pFileContext->filename);
	}
	else if (!fileExists(src_full_filename))
	{
		logWarning("file: "__FILE__", line: %d, " \
			"client ip: %s, source data file: %s " \
			"not exists, ignore it", __LINE__, \
			pTask->client_ip, src_full_filename);
	}
	else if (symlink(src_full_filename, pFileContext->filename) != 0)
	{
		result = errno != 0 ? errno : EPERM;
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, link file %s to %s fail, " \
			"errno: %d, error info: %s", \
			__LINE__, pTask->client_ip, \
			src_full_filename, pFileContext->filename, \
			result, STRERROR(result));
		break;
	}

	result = storage_binlog_write(pFileContext->timestamp2log, \
			STORAGE_OP_TYPE_REPLICA_CREATE_LINK, \
			pFileContext->fname2log);
	} while (0);


	CHECK_AND_WRITE_TO_STAT_FILE1

	pClientInfo->total_offset = 0;
	pTask->length = pClientInfo->total_length;
	pHeader = (TrackerHeader *)pTask->data;
	pHeader->status = result;
	pHeader->cmd = STORAGE_PROTO_CMD_RESP;
	long2buff(pClientInfo->total_length - sizeof(TrackerHeader), \
			pHeader->pkg_len);

	storage_nio_notify(pTask);

	return result;
}

static int storage_sync_link_file(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	char *p;
	char dest_filename[128];
	char dest_true_filename[128];
	int64_t nInPackLen;
	int dest_filename_len;
	int src_filename_len;
	int result;
	int dest_store_path_index;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	nInPackLen = pClientInfo->total_length - sizeof(TrackerHeader);

	if (nInPackLen <= 2 * FDFS_PROTO_PKG_LEN_SIZE + \
			4 + FDFS_GROUP_NAME_MAX_LEN)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, package size " \
			INT64_PRINTF_FORMAT"is not correct, " \
			"expect length > %d", __LINE__, \
			pTask->client_ip,  nInPackLen, \
			2 * FDFS_PROTO_PKG_LEN_SIZE + \
			4 + FDFS_GROUP_NAME_MAX_LEN);
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	p = pTask->data + sizeof(TrackerHeader);

	dest_filename_len = buff2long(p);
	p += FDFS_PROTO_PKG_LEN_SIZE;

	src_filename_len = buff2long(p);
	p += FDFS_PROTO_PKG_LEN_SIZE;

	if (dest_filename_len < 0 || dest_filename_len >= sizeof(dest_filename))
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, in request pkg, " \
			"filename length: %d is invalid, " \
			"which < 0 or >= %d", \
			__LINE__, pTask->client_ip, \
			dest_filename_len, (int)sizeof(dest_filename));
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	pFileContext->timestamp2log = buff2int(p);
	p += 4 + FDFS_GROUP_NAME_MAX_LEN;
	if (nInPackLen != 2 * FDFS_PROTO_PKG_LEN_SIZE + 4 + \
		FDFS_GROUP_NAME_MAX_LEN + dest_filename_len + src_filename_len)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, in request pkg, " \
			"pgk length: "INT64_PRINTF_FORMAT \
			" != bytes: %d", __LINE__, pTask->client_ip, \
			nInPackLen, 2 * FDFS_PROTO_PKG_LEN_SIZE + \
			FDFS_GROUP_NAME_MAX_LEN + dest_filename_len + \
			src_filename_len);
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	memcpy(dest_filename, p, dest_filename_len);
	*(dest_filename + dest_filename_len) = '\0';
	p += dest_filename_len;

	if ((result=storage_split_filename_ex(dest_filename, \
		&dest_filename_len, dest_true_filename, \
		&dest_store_path_index)) != 0)
	{
		pClientInfo->total_length = sizeof(TrackerHeader);
		return result;
	}
	if ((result=fdfs_check_data_filename(dest_true_filename, \
			dest_filename_len)) != 0)
	{
		pClientInfo->total_length = sizeof(TrackerHeader);
		return result;
	}
	snprintf(pFileContext->filename, sizeof(pFileContext->filename), \
			"%s/data/%s", g_store_paths[dest_store_path_index], \
			dest_true_filename);

	sprintf(pFileContext->fname2log, "%c"STORAGE_DATA_DIR_FORMAT"/%s", \
		FDFS_STORAGE_STORE_PATH_PREFIX_CHAR, \
		dest_store_path_index, dest_true_filename);

	pClientInfo->deal_func = storage_do_sync_link_file;

	pFileContext->fd = -1;
	pFileContext->op = FDFS_STORAGE_FILE_OP_WRITE;
	pFileContext->dio_thread_index = storage_dio_get_thread_index( \
		pTask, dest_store_path_index, pFileContext->op);

	if ((result=storage_dio_queue_push(pTask)) != 0)
	{
		pClientInfo->total_length = sizeof(TrackerHeader);
		return result;
	}

	return STORAGE_STATUE_DEAL_FILE;
}

/**
pkg format:
Header
FDFS_GROUP_NAME_MAX_LEN bytes: group_name
filename
**/
static int storage_server_get_metadata(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	char *p;
	int result;
	int store_path_index;
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	char true_filename[128];
	struct stat stat_buf;
	FDFSTrunkFullInfo trunkInfo;
	char *filename;
	int filename_len;
	int64_t file_bytes;
	int64_t nInPackLen;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	nInPackLen = pClientInfo->total_length - sizeof(TrackerHeader);
	pClientInfo->total_length = sizeof(TrackerHeader);

	if (nInPackLen <= FDFS_GROUP_NAME_MAX_LEN)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			INT64_PRINTF_FORMAT" is not correct, " \
			"expect length > %d", __LINE__, \
			STORAGE_PROTO_CMD_UPLOAD_FILE, \
			pTask->client_ip, nInPackLen, FDFS_GROUP_NAME_MAX_LEN);
		return EINVAL;
	}

	if (nInPackLen >= pTask->size)
	{
		logError("file: "__FILE__", line: %d, " \
				"cmd=%d, client ip: %s, package size " \
				INT64_PRINTF_FORMAT" is too large, " \
				"expect length should < %d", __LINE__, \
				STORAGE_PROTO_CMD_UPLOAD_FILE, \
				pTask->client_ip,  \
				nInPackLen, pTask->size);
		return EINVAL;
	}

	p = pTask->data + sizeof(TrackerHeader);

	memcpy(group_name, p, FDFS_GROUP_NAME_MAX_LEN);
	*(group_name + FDFS_GROUP_NAME_MAX_LEN) = '\0';
	p += FDFS_GROUP_NAME_MAX_LEN;
	if (strcmp(group_name, g_group_name) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, group_name: %s " \
			"not correct, should be: %s", \
			__LINE__, pTask->client_ip, \
			group_name, g_group_name);
		return EINVAL;
	}

	filename = p;
	filename_len = nInPackLen - FDFS_GROUP_NAME_MAX_LEN;
	*(filename + filename_len) = '\0';
	if ((result=storage_split_filename_ex(filename, \
		&filename_len, true_filename, &store_path_index)) != 0)
	{
		return result;
	}
	if ((result=fdfs_check_data_filename(true_filename, \
			filename_len)) != 0)
	{
		return result;
	}

	if ((result=trunk_file_stat(store_path_index, \
		true_filename, filename_len, &stat_buf, &trunkInfo)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call stat fail, logic file: %s, "\
			"error no: %d, error info: %s", \
			__LINE__, filename, \
			result, STRERROR(result));
		return result;
	}

	sprintf(pFileContext->filename, "%s/data/%s%s", \
			g_store_paths[store_path_index], \
			true_filename, STORAGE_META_FILE_EXT);
	if (lstat(pFileContext->filename, &stat_buf) == 0)
	{
		if (!S_ISREG(stat_buf.st_mode))
		{
			logError("file: "__FILE__", line: %d, " \
				"%s is not a regular file", \
				__LINE__, pFileContext->filename);
			return EISDIR;
		}

		file_bytes = stat_buf.st_size;
	}
	else
	{
		result = errno != 0 ? errno : ENOENT;
		if (result != ENOENT)
		{
			logError("file: "__FILE__", line: %d, " \
				"call stat fail, file: %s, "\
				"error no: %d, error info: %s", \
				__LINE__, pFileContext->filename, \
				result, STRERROR(result));
		}

		return result;
	}

	return storage_read_from_file(pTask, 0, file_bytes, \
			storage_get_metadata_done_callback, store_path_index);
}

/**
pkg format:
Header
8 bytes: file offset
8 bytes: download file bytes
FDFS_GROUP_NAME_MAX_LEN bytes: group_name
filename
**/
static int storage_server_download_file(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	char *p;
	int result;
	int store_path_index;
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	char true_filename[128];
	char *filename;
	int filename_len;
	int64_t file_offset;
	int64_t download_bytes;
	int64_t file_bytes;
	struct stat stat_buf;
	int64_t nInPackLen;
	FDFSTrunkFullInfo trunkInfo;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	nInPackLen = pClientInfo->total_length - sizeof(TrackerHeader);
	pClientInfo->total_length = sizeof(TrackerHeader);

	if (nInPackLen <= 16 + FDFS_GROUP_NAME_MAX_LEN)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			INT64_PRINTF_FORMAT" is not correct, " \
			"expect length > %d", __LINE__, \
			STORAGE_PROTO_CMD_UPLOAD_FILE, \
			pTask->client_ip,  \
			nInPackLen, 16 + FDFS_GROUP_NAME_MAX_LEN);
		return EINVAL;
	}

	if (nInPackLen >= pTask->size)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			INT64_PRINTF_FORMAT" is too large, " \
			"expect length should < %d", __LINE__, \
			STORAGE_PROTO_CMD_UPLOAD_FILE, \
			pTask->client_ip,  \
			nInPackLen, pTask->size);
		return EINVAL;
	}

	p = pTask->data + sizeof(TrackerHeader);

	file_offset = buff2long(p);
	p += FDFS_PROTO_PKG_LEN_SIZE;
	download_bytes = buff2long(p);
	p += FDFS_PROTO_PKG_LEN_SIZE;
	if (file_offset < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, invalid file offset: " \
			INT64_PRINTF_FORMAT,  __LINE__, \
			pTask->client_ip, file_offset);
		return EINVAL;
	}
	if (download_bytes < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, invalid download file bytes: " \
			INT64_PRINTF_FORMAT,  __LINE__, \
			pTask->client_ip, download_bytes);
		return EINVAL;
	}

	memcpy(group_name, p, FDFS_GROUP_NAME_MAX_LEN);
	*(group_name + FDFS_GROUP_NAME_MAX_LEN) = '\0';
	p += FDFS_GROUP_NAME_MAX_LEN;
	if (strcmp(group_name, g_group_name) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, group_name: %s " \
			"not correct, should be: %s", \
			__LINE__, pTask->client_ip, \
			group_name, g_group_name);
		return EINVAL;
	}

	filename = p;
	filename_len = nInPackLen - (16 + FDFS_GROUP_NAME_MAX_LEN);
	*(filename + filename_len) = '\0';

	if ((result=storage_split_filename_ex(filename, \
		&filename_len, true_filename, &store_path_index)) != 0)
	{
		return result;
	}
	if ((result=fdfs_check_data_filename(true_filename, filename_len)) != 0)
	{
		return result;
	}

	if ((result=trunk_file_stat(store_path_index, \
		true_filename, filename_len, &stat_buf, &trunkInfo)) == 0)
	{
		if (!S_ISREG(stat_buf.st_mode))
		{
			logError("file: "__FILE__", line: %d, " \
				"logic file %s is not a regular file", \
				__LINE__, filename);
			return EISDIR;
		}

		file_bytes = stat_buf.st_size;
	}
	else
	{
		file_bytes = 0;
		logError("file: "__FILE__", line: %d, " \
			"call stat fail, file: %s, "\
			"error no: %d, error info: %s", \
			__LINE__, pFileContext->filename, \
			result, STRERROR(result));
		return result;
	}

	if (download_bytes == 0)
	{
		download_bytes  = file_bytes - file_offset;
	}
	else if (download_bytes > file_bytes - file_offset)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, invalid download file bytes: " \
			INT64_PRINTF_FORMAT" > file remain bytes: " \
			INT64_PRINTF_FORMAT,  __LINE__, \
			pTask->client_ip, download_bytes, \
			file_bytes - file_offset);
		return EINVAL;
	}

	if (STORAGE_IS_TRUNK_FILE(trunkInfo))
	{
		trunk_get_full_filename((&trunkInfo), pFileContext->filename, \
				sizeof(pFileContext->filename));
		file_offset += TRUNK_FILE_START_OFFSET(trunkInfo);
	}
	else
	{
		sprintf(pFileContext->filename, "%s/data/%s", \
			g_store_paths[store_path_index], true_filename);
	}

	return storage_read_from_file(pTask, file_offset, download_bytes, \
			storage_download_file_done_callback, store_path_index);
}

static int storage_do_delete_file(struct fast_task_info *pTask, \
		DeleteFileLogCallback log_callback, \
		FileDealDoneCallback done_callback, \
		const int store_path_index)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	int result;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	pFileContext->fd = -1;
	pFileContext->op = FDFS_STORAGE_FILE_OP_DELETE;
	pFileContext->dio_thread_index = storage_dio_get_thread_index( \
		pTask, store_path_index, pFileContext->op);
	pFileContext->log_callback = log_callback;
	pFileContext->done_callback = done_callback;

	if ((result=storage_dio_queue_push(pTask)) != 0)
	{
		return result;
	}

	return STORAGE_STATUE_DEAL_FILE;
}

static int storage_read_from_file(struct fast_task_info *pTask, \
		const int64_t file_offset, const int64_t download_bytes, \
		FileDealDoneCallback done_callback, \
		const int store_path_index)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	TrackerHeader *pHeader;
	int result;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	pClientInfo->deal_func = dio_read_file;
	pClientInfo->clean_func = dio_read_finish_clean_up;
	pClientInfo->total_length = sizeof(TrackerHeader) + download_bytes;
	pClientInfo->total_offset = 0;

	pFileContext->fd = -1;
	pFileContext->op = FDFS_STORAGE_FILE_OP_READ;
	pFileContext->open_flags = O_RDONLY | extra_open_flags;
	pFileContext->offset = file_offset;
	pFileContext->start = file_offset;
	pFileContext->end = file_offset + download_bytes;
	pFileContext->dio_thread_index = storage_dio_get_thread_index( \
		pTask, store_path_index, pFileContext->op);
	pFileContext->done_callback = done_callback;

	pTask->length = sizeof(TrackerHeader);

	pHeader = (TrackerHeader *)pTask->data;
	pHeader->status = 0;
	pHeader->cmd = STORAGE_PROTO_CMD_RESP;
	long2buff(download_bytes, pHeader->pkg_len);

	if ((result=storage_dio_queue_push(pTask)) != 0)
	{
		pClientInfo->total_length = sizeof(TrackerHeader);
		return result;
	}

	return STORAGE_STATUE_DEAL_FILE;
}

static int storage_write_to_file(struct fast_task_info *pTask, \
		const int64_t file_offset, const int64_t upload_bytes, \
		const int buff_offset, TaskDealFunc deal_func, \
		FileDealDoneCallback done_callback, \
		DisconnectCleanFunc clean_func, const int store_path_index)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	int result;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	pClientInfo->deal_func = deal_func;
	pClientInfo->clean_func = clean_func;

	pFileContext->fd = -1;
	pFileContext->buff_offset = buff_offset;
	pFileContext->offset = file_offset;
	pFileContext->start = file_offset;
	pFileContext->end = file_offset + upload_bytes;
	pFileContext->dio_thread_index = storage_dio_get_thread_index( \
		pTask, store_path_index, pFileContext->op);
	pFileContext->done_callback = done_callback;

	if (pFileContext->calc_crc32)
	{
		pFileContext->crc32 = CRC32_XINIT;
	}

	if (pFileContext->calc_file_hash)
	{
		INIT_HASH_CODES4(pFileContext->file_hash_codes)
	}

	if ((result=storage_dio_queue_push(pTask)) != 0)
	{
		pClientInfo->total_length = sizeof(TrackerHeader);
		return result;
	}

	return STORAGE_STATUE_DEAL_FILE;
}

/**
pkg format:
Header
4 bytes: source delete timestamp
FDFS_GROUP_NAME_MAX_LEN bytes: group_name
filename
**/
static int storage_sync_delete_file(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	char *p;
	struct stat stat_buf;
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	char true_filename[128];
	char *filename;
	int filename_len;
	int result;
	int store_path_index;
	int64_t nInPackLen;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	nInPackLen = pClientInfo->total_length - sizeof(TrackerHeader);
	pClientInfo->total_length = sizeof(TrackerHeader);

	if (nInPackLen <= 4 + FDFS_GROUP_NAME_MAX_LEN)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			INT64_PRINTF_FORMAT" is not correct, " \
			"expect length <= %d", __LINE__, \
			STORAGE_PROTO_CMD_SYNC_DELETE_FILE, \
			pTask->client_ip,  \
			nInPackLen, 4 + FDFS_GROUP_NAME_MAX_LEN);
		return EINVAL;
	}

	if (nInPackLen >= pTask->size)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			INT64_PRINTF_FORMAT" is too large, " \
			"expect length should < %d", __LINE__, \
			STORAGE_PROTO_CMD_SYNC_DELETE_FILE, \
			pTask->client_ip,  nInPackLen, pTask->size);
		return EINVAL;
	}

	p = pTask->data + sizeof(TrackerHeader);
	pFileContext->timestamp2log = buff2int(p);
	p += 4;
	memcpy(group_name, p, FDFS_GROUP_NAME_MAX_LEN);
	*(group_name + FDFS_GROUP_NAME_MAX_LEN) = '\0';
	p += FDFS_GROUP_NAME_MAX_LEN;
	if (strcmp(group_name, g_group_name) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, group_name: %s " \
			"not correct, should be: %s", \
			__LINE__, pTask->client_ip, \
			group_name, g_group_name);
		return EINVAL;
	}

	filename = p;
	filename_len = nInPackLen - (4 + FDFS_GROUP_NAME_MAX_LEN);
	*(filename + filename_len) = '\0';
	if ((result=storage_split_filename_ex(filename, \
		&filename_len, true_filename, &store_path_index)) != 0)
	{
		return result;
	}
	if ((result=fdfs_check_data_filename(true_filename, filename_len)) != 0)
	{
		return result;
	}

	if ((result=trunk_file_lstat(store_path_index, true_filename, \
			filename_len, &stat_buf, \
			&(pFileContext->extra_info.upload.trunk_info))) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, stat logic file %s fail, " \
			"errno: %d, error info: %s.", \
			__LINE__, pTask->client_ip, \
			filename, result, STRERROR(result));
		return result;
	}
	if (S_ISREG(stat_buf.st_mode))
	{
		pFileContext->delete_flag = STORAGE_DELETE_FLAG_FILE;
	}
	else if (S_ISLNK(stat_buf.st_mode))
	{
		pFileContext->delete_flag = STORAGE_DELETE_FLAG_LINK;
	}
	else
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, logic file %s is NOT a file", \
			__LINE__, pTask->client_ip, filename);
		return EINVAL;
	}

	if (STORAGE_IS_TRUNK_FILE(pFileContext->extra_info.upload.trunk_info))
	{
		pClientInfo->deal_func = dio_delete_trunk_file;
		trunk_get_full_filename((&pFileContext->extra_info.upload.\
				trunk_info), pFileContext->filename, \
				sizeof(pFileContext->filename));
	}
	else
	{
		pClientInfo->deal_func = dio_delete_normal_file;
		sprintf(pFileContext->filename, "%s/data/%s", \
			g_store_paths[store_path_index], true_filename);
	}

	strcpy(pFileContext->fname2log, filename);
	pFileContext->sync_flag = STORAGE_OP_TYPE_REPLICA_DELETE_FILE;
	return storage_do_delete_file(pTask, storage_sync_delete_file_log_error, \
		storage_sync_delete_file_done_callback, store_path_index);
}

/**
pkg format:
Header
FDFS_GROUP_NAME_MAX_LEN bytes: group_name
filename
**/
static int storage_server_delete_file(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	char *p;
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	char true_filename[128];
	char *filename;
	int filename_len;
	struct stat stat_buf;
	int result;
	int store_path_index;
	int64_t nInPackLen;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	nInPackLen = pClientInfo->total_length - sizeof(TrackerHeader);
	pClientInfo->total_length = sizeof(TrackerHeader);

	pFileContext->delete_flag = STORAGE_DELETE_FLAG_NONE;
	if (nInPackLen <= FDFS_GROUP_NAME_MAX_LEN)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			INT64_PRINTF_FORMAT" is not correct, " \
			"expect length <= %d", __LINE__, \
			STORAGE_PROTO_CMD_UPLOAD_FILE, \
			pTask->client_ip,  \
			nInPackLen, FDFS_GROUP_NAME_MAX_LEN);
		return EINVAL;
	}

	if (nInPackLen >= pTask->size)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			INT64_PRINTF_FORMAT" is too large, " \
			"expect length should < %d", __LINE__, \
			STORAGE_PROTO_CMD_UPLOAD_FILE, \
			pTask->client_ip,  nInPackLen, pTask->size);
		return EINVAL;
	}

	p = pTask->data + sizeof(TrackerHeader);
	memcpy(group_name, p, FDFS_GROUP_NAME_MAX_LEN);
	*(group_name + FDFS_GROUP_NAME_MAX_LEN) = '\0';
	p += FDFS_GROUP_NAME_MAX_LEN;
	if (strcmp(group_name, g_group_name) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, group_name: %s " \
			"not correct, should be: %s", \
			__LINE__, pTask->client_ip, \
			group_name, g_group_name);
		return EINVAL;
	}

	filename = p;
	filename_len = nInPackLen - FDFS_GROUP_NAME_MAX_LEN;
	*(filename + filename_len) = '\0';

	if ((result=storage_split_filename_ex(filename, \
		&filename_len, true_filename, &store_path_index)) != 0)
	{
		return result;
	}
	if ((result=fdfs_check_data_filename(true_filename, filename_len)) != 0)
	{
		return result;
	}

	if ((result=trunk_file_lstat(store_path_index, true_filename, \
			filename_len, &stat_buf, \
			&(pFileContext->extra_info.upload.trunk_info))) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, stat logic file %s fail, " \
			"errno: %d, error info: %s", \
			__LINE__, pTask->client_ip, \
			filename, result, STRERROR(result));
		return result;
	}
	if (S_ISREG(stat_buf.st_mode))
	{
		pFileContext->delete_flag |= STORAGE_DELETE_FLAG_FILE;
	}
	else if (S_ISLNK(stat_buf.st_mode))
	{
		pFileContext->delete_flag |= STORAGE_DELETE_FLAG_LINK;
	}
	else
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, file %s is NOT a file", \
			__LINE__, pTask->client_ip, \
			pFileContext->filename);
		return EINVAL;
	}

	if (STORAGE_IS_TRUNK_FILE(pFileContext->extra_info.upload.trunk_info))
	{
		pFileContext->extra_info.upload.if_trunk_file = true;
		pClientInfo->deal_func = dio_delete_trunk_file;
		trunk_get_full_filename((&pFileContext->extra_info.upload.\
				trunk_info), pFileContext->filename, \
				sizeof(pFileContext->filename));
	}
	else
	{
		pFileContext->extra_info.upload.if_trunk_file = false;
		pClientInfo->deal_func = dio_delete_normal_file;
		sprintf(pFileContext->filename, "%s/data/%s", \
			g_store_paths[store_path_index], true_filename);
	}

	strcpy(pFileContext->fname2log, filename);
	return storage_do_delete_file(pTask, storage_delete_file_log_error, \
			storage_delete_fdfs_file_done_callback, \
			store_path_index);
}

static int storage_create_link_core(struct fast_task_info *pTask, \
		const int store_path_index, SourceFileInfo *pSourceFileInfo, \
		const char *src_filename, const char *master_filename, \
		const int master_filename_len, \
		const char *prefix_name, const char *file_ext_name, \
		char *filename, int *filename_len)
{
	StorageClientInfo *pClientInfo;
	int result;
	struct stat stat_buf;
	char true_filename[128];
	char src_full_filename[MAX_PATH_SIZE+64];
	char full_filename[MAX_PATH_SIZE];

	pClientInfo = (StorageClientInfo *)pTask->arg;
	do
	{
	sprintf(src_full_filename, "%s/data/%s", \
		g_store_paths[store_path_index], \
		pSourceFileInfo->src_true_filename);

	if (lstat(src_full_filename, &stat_buf) != 0 || \
		!S_ISREG(stat_buf.st_mode))
	{
		FDHTKeyInfo key_info;
		GroupArray *pGroupArray;

		result = errno != 0 ? errno : EINVAL;
		logError("file: "__FILE__", line: %d, " \
				"client ip: %s, file: %s call stat fail " \
				"or it is not a regular file, " \
				"errno: %d, error info: %s", \
				__LINE__, pTask->client_ip, \
				src_full_filename, \
				result, STRERROR(result));


		if (g_check_file_duplicate)
		{
			pGroupArray=&((g_nio_thread_data+pClientInfo->nio_thread_index)\
					->group_array);
			//clean invalid entry
			memset(&key_info, 0, sizeof(key_info));
			key_info.namespace_len = g_namespace_len;
			memcpy(key_info.szNameSpace, g_key_namespace, \
					g_namespace_len);

			key_info.obj_id_len = pSourceFileInfo->src_file_sig_len;
			memcpy(key_info.szObjectId, pSourceFileInfo->src_file_sig,
					key_info.obj_id_len);
			key_info.key_len = sizeof(FDHT_KEY_NAME_FILE_ID) - 1;
			memcpy(key_info.szKey, FDHT_KEY_NAME_FILE_ID, \
					sizeof(FDHT_KEY_NAME_FILE_ID) - 1);
			fdht_delete_ex(pGroupArray, g_keep_alive, &key_info);

			key_info.obj_id_len = snprintf(key_info.szObjectId, \
					sizeof(key_info.szObjectId), "%s/%s", \
					g_group_name, src_filename);
			key_info.key_len = sizeof(FDHT_KEY_NAME_REF_COUNT) - 1;
			memcpy(key_info.szKey, FDHT_KEY_NAME_REF_COUNT, \
					key_info.key_len);
			fdht_delete_ex(pGroupArray, g_keep_alive, &key_info);
		}

		break;
	}

	if (master_filename_len > 0)
	{
		int master_store_path_index;

		*filename_len = master_filename_len;
		if ((result=storage_split_filename_ex( \
			master_filename, filename_len, true_filename, \
			&master_store_path_index)) != 0)
		{
			break;
		}

		if (master_store_path_index != store_path_index)
		{
			logError("file: "__FILE__", line: %d, " \
				"client ip:%s, invalid master store " \
				"path index: %d != source store path " \
				"index: %d", __LINE__, pTask->client_ip, \
				master_store_path_index, store_path_index);
			result = EINVAL;
			break;
		}

		if ((result=fdfs_check_data_filename(true_filename, \
					*filename_len)) != 0)
		{
			break;
		}

		if ((result=fdfs_gen_slave_filename( \
			true_filename, prefix_name, file_ext_name, \
			filename, filename_len)) != 0)
		{
			break;
		}

		snprintf(full_filename, sizeof(full_filename), \
			"%s/data/%s", g_store_paths[store_path_index], \
			filename);
		if (fileExists(full_filename))
		{
			logError("file: "__FILE__", line: %d, " \
				"client ip: %s, slave file: %s " \
				"already exist", __LINE__, \
				pTask->client_ip, full_filename);
			result = EEXIST;
			break;
		}
	}
	else
	{
		*filename = '\0';
		*filename_len = 0;
	}

	result = storage_service_do_create_link(pTask, store_path_index, \
			pSourceFileInfo, stat_buf.st_size, \
			master_filename, prefix_name, file_ext_name, \
			filename, filename_len);
	if (result != 0)
	{
		break;
	}

	result = storage_binlog_write(time(NULL), \
			STORAGE_OP_TYPE_SOURCE_CREATE_LINK, filename);
	} while (0);

	if (result == 0)
	{
		CHECK_AND_WRITE_TO_STAT_FILE3( \
			g_storage_stat.total_create_link_count, \
			g_storage_stat.success_create_link_count, \
			g_storage_stat.last_source_update)
	}
	else
	{
		pthread_mutex_lock(&stat_count_thread_lock);
		g_storage_stat.total_create_link_count++;
		pthread_mutex_unlock(&stat_count_thread_lock);
	}

	return result;
}

/**
pkg format:
Header
8 bytes: master filename len
8 bytes: source filename len
8 bytes: source file signature len
FDFS_GROUP_NAME_MAX_LEN bytes: group_name
FDFS_FILE_PREFIX_MAX_LEN bytes  : filename prefix, can be empty
FDFS_FILE_EXT_NAME_MAX_LEN bytes: file ext name, do not include dot (.)
master filename len: master filename
source filename len: source filename
source file signature len: source file signature
**/
static int storage_do_create_link(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	TrackerHeader *pHeader;
	char *p;
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	char prefix_name[FDFS_FILE_PREFIX_MAX_LEN + 1];
	char file_ext_name[FDFS_FILE_EXT_NAME_MAX_LEN + 1];
	char src_filename[128];
	char master_filename[128];
	char filename[128];
	int src_filename_len;
	int master_filename_len;
	int filename_len;
	int len;
	int result;
	int store_path_index;
	SourceFileInfo sourceFileInfo;
	int64_t nInPackLen;

	pClientInfo = (StorageClientInfo *)pTask->arg;

	nInPackLen = pClientInfo->total_length - sizeof(TrackerHeader);
	pClientInfo->total_length = sizeof(TrackerHeader);

	do
	{
	if (nInPackLen <= 3 * FDFS_PROTO_PKG_LEN_SIZE + \
		FDFS_GROUP_NAME_MAX_LEN + FDFS_FILE_PREFIX_MAX_LEN + \
		FDFS_FILE_EXT_NAME_MAX_LEN)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			INT64_PRINTF_FORMAT" is not correct, " \
			"expect length > %d", __LINE__, \
			STORAGE_PROTO_CMD_UPLOAD_FILE, pTask->client_ip, \
			 nInPackLen, 4 * FDFS_PROTO_PKG_LEN_SIZE + \
			FDFS_GROUP_NAME_MAX_LEN + FDFS_FILE_PREFIX_MAX_LEN + \
			FDFS_FILE_EXT_NAME_MAX_LEN);
		result = EINVAL;
		break;
	}

	p = pTask->data + sizeof(TrackerHeader);
	master_filename_len = buff2long(p);
	p += FDFS_PROTO_PKG_LEN_SIZE;
	src_filename_len = buff2long(p);
	p += FDFS_PROTO_PKG_LEN_SIZE;
	sourceFileInfo.src_file_sig_len = buff2long(p);
	p += FDFS_PROTO_PKG_LEN_SIZE;
	if (master_filename_len < 0 || master_filename_len >= \
			sizeof(master_filename))
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, pkg length is not correct, " \
			"invalid master filename length: %d", \
			__LINE__, pTask->client_ip, master_filename_len);
		result = EINVAL;
		break;
	}

	if (src_filename_len <= 0 || src_filename_len >= \
			sizeof(src_filename))
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, pkg length is not correct, " \
			"invalid filename length: %d", \
			__LINE__, pTask->client_ip, src_filename_len);
		result = EINVAL;
		break;
	}

	if (sourceFileInfo.src_file_sig_len <= 0 || \
		sourceFileInfo.src_file_sig_len >= \
			sizeof(sourceFileInfo.src_file_sig))
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, pkg length is not correct, " \
			"invalid file signature length: %d", \
			__LINE__, pTask->client_ip, \
			sourceFileInfo.src_file_sig_len);
		result = EINVAL;
		break;
	}

	if (sourceFileInfo.src_file_sig_len != nInPackLen - \
		(3 * FDFS_PROTO_PKG_LEN_SIZE+FDFS_GROUP_NAME_MAX_LEN + \
		 FDFS_FILE_PREFIX_MAX_LEN + FDFS_FILE_EXT_NAME_MAX_LEN +\
		 master_filename_len + src_filename_len))
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, pkg length is not correct, " \
			"invalid src_file_sig_len : %d", \
			__LINE__, pTask->client_ip, \
			sourceFileInfo.src_file_sig_len);
		result = EINVAL;
		break;
	}

	memcpy(group_name, p, FDFS_GROUP_NAME_MAX_LEN);
	*(group_name + FDFS_GROUP_NAME_MAX_LEN) = '\0';
	p += FDFS_GROUP_NAME_MAX_LEN;
	if (strcmp(group_name, g_group_name) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, group_name: %s " \
			"not correct, should be: %s", \
			__LINE__, pTask->client_ip, \
			group_name, g_group_name);
		result = EINVAL;
		break;
	}

	memcpy(prefix_name, p, FDFS_FILE_PREFIX_MAX_LEN);
	*(prefix_name + FDFS_FILE_PREFIX_MAX_LEN) = '\0';
	p += FDFS_FILE_PREFIX_MAX_LEN;

	memcpy(file_ext_name, p, FDFS_FILE_EXT_NAME_MAX_LEN);
	*(file_ext_name + FDFS_FILE_EXT_NAME_MAX_LEN) = '\0';
	p += FDFS_FILE_EXT_NAME_MAX_LEN;

	len = master_filename_len + src_filename_len + \
	      sourceFileInfo.src_file_sig_len;
	if (len > 256)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip:%s, invalid pkg length, " \
			"file relative length: %d > %d", \
			__LINE__, pTask->client_ip, len, 256);
		result = EINVAL;
		break;
	}

	if (master_filename_len > 0)
	{
		memcpy(master_filename, p, master_filename_len);
		*(master_filename + master_filename_len) = '\0';
		p += master_filename_len;
	}
	else
	{
		*master_filename = '\0';
	}

	memcpy(src_filename, p, src_filename_len);
	*(src_filename + src_filename_len) = '\0';
	p += src_filename_len;

	memcpy(sourceFileInfo.src_file_sig, p, sourceFileInfo.src_file_sig_len);
	*(sourceFileInfo.src_file_sig + sourceFileInfo.src_file_sig_len) = '\0';

	if ((result=storage_split_filename_ex(src_filename, \
		&src_filename_len, sourceFileInfo.src_true_filename, \
		&store_path_index)) != 0)
	{
		break;
	}
	if ((result=fdfs_check_data_filename( \
		sourceFileInfo.src_true_filename, src_filename_len)) != 0)
	{
		break;
	}

	result = storage_create_link_core(pTask, \
			store_path_index, &sourceFileInfo, src_filename, \
			master_filename, master_filename_len, \
			prefix_name, file_ext_name, \
			filename, &filename_len);
	} while (0);

	if (result == 0)
	{
		pClientInfo->total_length += FDFS_GROUP_NAME_MAX_LEN + filename_len;
		p = pTask->data + sizeof(TrackerHeader);
		memcpy(p, g_group_name, FDFS_GROUP_NAME_MAX_LEN);
		memcpy(p + FDFS_GROUP_NAME_MAX_LEN, filename, filename_len);
	}

	pClientInfo->total_offset = 0;
	pTask->length = pClientInfo->total_length;
	pHeader = (TrackerHeader *)pTask->data;
	pHeader->status = result;
	pHeader->cmd = STORAGE_PROTO_CMD_RESP;
	long2buff(pClientInfo->total_length - sizeof(TrackerHeader), \
			pHeader->pkg_len);

	storage_nio_notify(pTask);
	return result;
}

static int storage_create_link(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	char *p;
	char src_filename[128];
	char src_true_filename[128];
	int src_filename_len;
	int result;
	int store_path_index;
	int64_t nInPackLen;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext =  &(pClientInfo->file_context);

	nInPackLen = pClientInfo->total_length - sizeof(TrackerHeader);

	if (nInPackLen <= 3 * FDFS_PROTO_PKG_LEN_SIZE + \
		FDFS_GROUP_NAME_MAX_LEN + FDFS_FILE_PREFIX_MAX_LEN + \
		FDFS_FILE_EXT_NAME_MAX_LEN)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip: %s, package size " \
			INT64_PRINTF_FORMAT" is not correct, " \
			"expect length > %d", __LINE__, \
			STORAGE_PROTO_CMD_UPLOAD_FILE, pTask->client_ip, \
			 nInPackLen, 4 * FDFS_PROTO_PKG_LEN_SIZE + \
			FDFS_GROUP_NAME_MAX_LEN + FDFS_FILE_PREFIX_MAX_LEN + \
			FDFS_FILE_EXT_NAME_MAX_LEN);
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	p = pTask->data + sizeof(TrackerHeader) + FDFS_PROTO_PKG_LEN_SIZE;
	src_filename_len = buff2long(p);
	if (src_filename_len <= 0 || src_filename_len >= \
			sizeof(src_filename))
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, pkg length is not correct, " \
			"invalid filename length: %d", \
			__LINE__, pTask->client_ip, src_filename_len);
		pClientInfo->total_length = sizeof(TrackerHeader);
		return EINVAL;
	}

	p += 2 * FDFS_PROTO_PKG_LEN_SIZE + FDFS_GROUP_NAME_MAX_LEN + \
		FDFS_FILE_PREFIX_MAX_LEN + FDFS_FILE_EXT_NAME_MAX_LEN;
	memcpy(src_filename, p, src_filename_len);
	*(src_filename + src_filename_len) = '\0';

	if ((result=storage_split_filename_ex(src_filename, \
		&src_filename_len, src_true_filename, &store_path_index)) != 0)
	{
		pClientInfo->total_length = sizeof(TrackerHeader);
		return result;
	}

	pClientInfo->deal_func = storage_do_create_link;

	pFileContext->fd = -1;
	pFileContext->op = FDFS_STORAGE_FILE_OP_WRITE;
	pFileContext->dio_thread_index = storage_dio_get_thread_index( \
		pTask, store_path_index, pFileContext->op);

	if ((result=storage_dio_queue_push(pTask)) != 0)
	{
		pClientInfo->total_length = sizeof(TrackerHeader);
		return result;
	}

	return STORAGE_STATUE_DEAL_FILE;
}

int fdfs_stat_file_sync_func(void *args)
{
	int result;

	if (last_stat_change_count !=  g_stat_change_count)
	{
		if ((result=storage_write_to_stat_file()) == 0)
		{
			last_stat_change_count = g_stat_change_count;
		}

		return result;
	}
	else
	{
		return 0;
	}
}

int storage_deal_task(struct fast_task_info *pTask)
{
	TrackerHeader *pHeader;
	StorageClientInfo *pClientInfo;
	int result;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pHeader = (TrackerHeader *)pTask->data;

	switch(pHeader->cmd)
	{
		case STORAGE_PROTO_CMD_DOWNLOAD_FILE:
			result = storage_server_download_file(pTask);
			break;
		case STORAGE_PROTO_CMD_GET_METADATA:
			result = storage_server_get_metadata(pTask);
			break;
		case STORAGE_PROTO_CMD_UPLOAD_FILE:
			result = storage_upload_file(pTask, false);
			break;
		case STORAGE_PROTO_CMD_UPLOAD_APPENDER_FILE:
			result = storage_upload_file(pTask, true);
			break;
		case STORAGE_PROTO_CMD_APPEND_FILE:
			result = storage_append_file(pTask);
			break;
		case STORAGE_PROTO_CMD_UPLOAD_SLAVE_FILE:
			result = storage_upload_slave_file(pTask);
			break;
		case STORAGE_PROTO_CMD_DELETE_FILE:
			result = storage_server_delete_file(pTask);
			break;
		case STORAGE_PROTO_CMD_CREATE_LINK:
			result = storage_create_link(pTask);
			break;
		case STORAGE_PROTO_CMD_SYNC_CREATE_FILE:
			result = storage_sync_copy_file(pTask, pHeader->cmd);
			break;
		case STORAGE_PROTO_CMD_SYNC_DELETE_FILE:
			result = storage_sync_delete_file(pTask);
			break;
		case STORAGE_PROTO_CMD_SYNC_UPDATE_FILE:
			result = storage_sync_copy_file(pTask, pHeader->cmd);
			break;
		case STORAGE_PROTO_CMD_SYNC_APPEND_FILE:
			result = storage_sync_append_file(pTask);
			break;
		case STORAGE_PROTO_CMD_SYNC_CREATE_LINK:
			result = storage_sync_link_file(pTask);
			break;
		case STORAGE_PROTO_CMD_SET_METADATA:
			result = storage_server_set_metadata(pTask);
			break;
		case STORAGE_PROTO_CMD_QUERY_FILE_INFO:
			result = storage_server_query_file_info(pTask);
			break;
		case STORAGE_PROTO_CMD_FETCH_ONE_PATH_BINLOG:
			result = storage_server_fetch_one_path_binlog(pTask);
			break;
		case FDFS_PROTO_CMD_QUIT:
			task_finish_clean_up(pTask);
			return 0;
		case FDFS_PROTO_CMD_ACTIVE_TEST:
			result = storage_deal_active_test(pTask);
			break;
		case STORAGE_PROTO_CMD_REPORT_CLIENT_IP:
			result = storage_server_report_client_ip(pTask);
			break;
		case STORAGE_PROTO_CMD_TRUNK_ALLOC_SPACE:
			result = storage_server_trunk_alloc_space(pTask);
			break;
		case STORAGE_PROTO_CMD_TRUNK_ALLOC_CONFIRM:
			result = storage_server_trunk_alloc_confirm(pTask);
			break;
		case STORAGE_PROTO_CMD_TRUNK_FREE_SPACE:
			result = storage_server_trunk_free_space(pTask);
			break;
		case STORAGE_PROTO_CMD_TRUNK_SYNC_BINLOG:
			result = storage_server_trunk_sync_binlog(pTask);
			break;
		default:
			logError("file: "__FILE__", line: %d, "  \
				"client ip: %s, unkown cmd: %d", \
				__LINE__, pTask->client_ip, \
				pHeader->cmd);
			result = EINVAL;
			break;
	}

	if (result != STORAGE_STATUE_DEAL_FILE)
	{
		pClientInfo->total_offset = 0;
		pTask->length = pClientInfo->total_length;

		pHeader = (TrackerHeader *)pTask->data;
		pHeader->status = result;
		pHeader->cmd = STORAGE_PROTO_CMD_RESP;
		long2buff(pClientInfo->total_length - sizeof(TrackerHeader), \
				pHeader->pkg_len);
		storage_send_add_event(pTask);
	}

	return result;
}

