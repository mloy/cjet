#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "compiler.h"
#include "cJSON.h"
#include "jet_config.h"
#include "parse.h"
#include "peer.h"

static int parse_json_rpc(cJSON *json_rpc, struct peer *p)
{
	/* TODO: check if there is a tag "jsonrpc" with value "2.0" */
	const char *method_string;
	int ret = 0;
	cJSON *method = cJSON_GetObjectItem(json_rpc, "method");

	if (unlikely(method == NULL)) {
		goto no_method;
	}
	method_string = method->valuestring;
	if (unlikely(method_string == NULL)) {
		goto no_method;
	}

	if (strcmp(method_string, "set") == 0) {

	} else if (strcmp(method_string, "post") == 0) {

	} else if (strcmp(method_string, "add") == 0) {

	} else if (strcmp(method_string, "remove") == 0) {

	} else if (strcmp(method_string, "call") == 0) {

	} else if (strcmp(method_string, "fetch") == 0) {

	} else if (strcmp(method_string, "unfetch") == 0) {

	} else if (strcmp(method_string, "config") == 0) {
		ret = process_config(json_rpc, p);
	} else {
		fprintf(stderr, "Unsupported method: %s!\n", method_string);
		ret = -1;
		goto unsupported_method;
	}
	return ret;

no_method:
	fprintf(stderr, "Cannot find supported method!\n");
unsupported_method:
	return ret;
}

int parse_message(const char *msg, uint32_t length, struct peer *p)
{
	cJSON *root;
	const char *end_parse;
	uint32_t parsed_length;
	int ret = 0;

	root = cJSON_ParseWithOpts(msg, &end_parse, 0);
	if (unlikely(root == NULL)) {
		fprintf(stderr, "Could not parse JSON!\n");
		return -1;
	}

	parsed_length = end_parse - msg;
	if (unlikely(parsed_length != length)) {
		fprintf(stderr, "length of parsed JSON (%d) does not match message length (%d)!\n", parsed_length, length);
		ret = -1;
		goto out;
	}

	switch (root->type) {
	case cJSON_Array: {
		int i;
		int array_size = cJSON_GetArraySize(root);
		for (i = 0; i < array_size; i++) {
			cJSON *sub_item = cJSON_GetArrayItem(root, i);
			if (likely(sub_item->type == cJSON_Object)) {
				ret = parse_json_rpc(sub_item, p);
				if (unlikely(ret == -1)) {
					goto out;
				}
			} else {
				fprintf(stderr, "JSON is not an object!\n");
				ret = -1;
				goto out;
			}
		}
	}
		break;

	case cJSON_Object:
		ret = parse_json_rpc(root, p);
		break;

	default:
		fprintf(stderr, "JSON is neither array or object!\n");
		ret = -1;
		goto out;
	}

out:
	cJSON_Delete(root);
	return ret;
}
