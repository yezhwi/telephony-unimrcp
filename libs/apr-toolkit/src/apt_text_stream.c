/*
 * Copyright 2008-2010 Arsen Chaloyan
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 * $Id$
 */

#include <stdlib.h>
#include <stdio.h>
#include <apr_uuid.h>
#include "apt_text_stream.h"

#define TOKEN_TRUE  "true"
#define TOKEN_FALSE "false"
#define TOKEN_TRUE_LENGTH  (sizeof(TOKEN_TRUE)-1)
#define TOKEN_FALSE_LENGTH (sizeof(TOKEN_FALSE)-1)


/** Navigate through the lines of the text stream (message) */
APT_DECLARE(apt_bool_t) apt_text_line_read(apt_text_stream_t *stream, apt_str_t *line)
{
	char *pos = stream->pos;
	apt_bool_t status = FALSE;
	line->length = 0;
	line->buf = pos;
	/* while not end of stream */
	while(pos < stream->end) {
		if(*pos == APT_TOKEN_CR) {
			/* end of line detected */
			line->length = pos - line->buf;
			pos++;
			if(pos < stream->end && *pos == APT_TOKEN_LF) {
				pos++;
			}
			status = TRUE;
			break;
		}
		else if(*pos == APT_TOKEN_LF) {
			/* end of line detected */
			line->length = pos - line->buf;
			pos++;
			status = TRUE;
			break;
		}
		pos++;
	}

	if(status == TRUE) {
		/* advance stream pos */
		stream->pos = pos;
	}
	else {
		/* end of stream is reached, do not advance stream pos, but set is_eos flag */
		stream->is_eos = TRUE;
		line->length = pos - line->buf;
	}
	return status;
}

/** To be used to navigate through the header fields (name:value pairs) of the text stream (message) 
	Valid header fields are:
		name:value<CRLF>
		name: value<CRLF>
		name:    value<CRLF>
		name: value<LF>
		name:<CRLF>              (only name, no value)
		<CRLF>                   (empty header)
	Malformed header fields are:
		name:value               (missing end of line <CRLF>)
		name<CRLF>               (missing separator ':')
*/
APT_DECLARE(apt_bool_t) apt_text_header_read(apt_text_stream_t *stream, apt_pair_t *pair)
{
	char *pos = stream->pos;
	apt_bool_t status = FALSE;
	apt_string_reset(&pair->name);
	apt_string_reset(&pair->value);
	/* while not end of stream */
	while(pos < stream->end) {
		if(*pos == APT_TOKEN_CR) {
			/* end of line detected */
			if(pair->value.buf) {
				/* set length of the value */
				pair->value.length = pos - pair->value.buf;
			}
			pos++;
			if(pos < stream->end && *pos == APT_TOKEN_LF) {
				pos++;
			}
			status = TRUE;
			break;
		}
		else if(*pos == APT_TOKEN_LF) {
			/* end of line detected */
			if(pair->value.buf) {
				/* set length of the value */
				pair->value.length = pos - pair->value.buf;
			}
			pos++;
			status = TRUE;
			break;
		}
		else if(!pair->name.length) {
			/* skip preceding white spaces (SHOULD NOT be any WSP, though) and read name */
			if(!pair->name.buf && apt_text_is_wsp(*pos) == FALSE) {
				pair->name.buf = pos;
			}
			if(*pos == ':') {
				/* set length of the name */
				pair->name.length = pos - pair->name.buf;
			}
		}
		else if(!pair->value.length) {
			/* skip preceding white spaces and read value */
			if(!pair->value.buf && apt_text_is_wsp(*pos) == FALSE) {
				pair->value.buf = pos;
			}
		}
		pos++;
	}

	if(status == TRUE) {
		/* advance stream pos regardless it's a valid header or not */
		stream->pos = pos;
		
		/* if length == 0 && buf => header is malformed */
		if(!pair->name.length && pair->name.buf) {
			status = FALSE;
		}
	}
	else {
		/* end of stream is reached, do not advance stream pos, but set is_eos flag */
		stream->is_eos = TRUE;
	}

	return status;
}


/** Navigate through the fields of the line */
APT_DECLARE(apt_bool_t) apt_text_field_read(apt_text_stream_t *stream, char separator, apt_bool_t skip_spaces, apt_str_t *field)
{
	char *pos = stream->pos;
	if(skip_spaces == TRUE) {
		while(pos < stream->end && *pos == APT_TOKEN_SP) pos++;
	}

	field->buf = pos;
	field->length = 0;
	while(pos < stream->end && *pos != separator) pos++;

	field->length = pos - field->buf;
	if(pos < stream->end) {
		/* skip the separator */
		pos++;
	}

	stream->pos = pos;
	return field->length ? TRUE : FALSE;
}

/** Scroll text stream */
APT_DECLARE(apt_bool_t) apt_text_stream_scroll(apt_text_stream_t *stream)
{
	if(stream->pos == stream->end) {
		stream->pos = stream->text.buf;
	}
	else {
		apr_size_t remaining_length = stream->text.buf + stream->text.length - stream->pos;
		if(!remaining_length || remaining_length == stream->text.length) {
			stream->pos = stream->text.buf + remaining_length;
			return FALSE;
		}
		memmove(stream->text.buf,stream->pos,remaining_length);
		stream->pos = stream->text.buf + remaining_length;
		stream->text.length = remaining_length;
	}
	*stream->pos = '\0';
	return TRUE;
}

/** Parse id@resource string */
APT_DECLARE(apt_bool_t) apt_id_resource_parse(const apt_str_t *str, char separator, apt_str_t *id, apt_str_t *resource, apr_pool_t *pool)
{
	apt_str_t field = *str;
	const char *pos = strchr(str->buf,separator);
	if(!pos) {
		return FALSE;
	}

	field.length = pos - field.buf;
	if(field.length >= str->length) {
		return FALSE;
	}
	apt_string_copy(id,&field,pool);
	field.buf += field.length + 1;
	field.length = str->length - (field.length + 1);
	apt_string_copy(resource,&field,pool);
	return TRUE;
}

/** Generate id@resource string */
APT_DECLARE(apt_bool_t) apt_id_resource_generate(const apt_str_t *id, const apt_str_t *resource, char separator, apt_str_t *str, apr_pool_t *pool)
{
	apr_size_t length = id->length+resource->length+1;
	char *buf = apr_palloc(pool,length+1);
	memcpy(buf,id->buf,id->length);
	buf[id->length] = separator;
	memcpy(buf+id->length+1,resource->buf,resource->length);
	buf[length] = '\0';
	str->buf = buf;
	str->length = length;
	return TRUE;
}

/** Generate name-value pair line */
APT_DECLARE(apt_bool_t) apt_text_name_value_insert(apt_text_stream_t *stream, const apt_str_t *name, const apt_str_t *value)
{
	char *pos = stream->pos;
	if(pos + name->length + value->length + 2 >= stream->end) {
		return FALSE;
	}
	memcpy(pos,name->buf,name->length);
	pos += name->length;
	*pos++ = ':';
	*pos++ = APT_TOKEN_SP;
	if(apt_string_is_empty(value) == FALSE) {
		memcpy(pos,value->buf,value->length);
		pos += value->length;
	}
	stream->pos = pos;
	return apt_text_eol_insert(stream);
}

/** Generate only the name ("name":) of the header field */
APT_DECLARE(apt_bool_t) apt_text_header_name_insert(apt_text_stream_t *stream, const apt_str_t *name)
{
	char *pos = stream->pos;
	if(pos + name->length + 2 >= stream->end) {
		return FALSE;
	}
	memcpy(pos,name->buf,name->length);
	pos += name->length;
	*pos++ = ':';
	*pos++ = APT_TOKEN_SP;
	stream->pos = pos;
	return TRUE;
}

/** Parse name=value pair */
static apt_bool_t apt_pair_parse(apt_pair_t *pair, const apt_str_t *field, apr_pool_t *pool)
{
	apt_text_stream_t stream;
	apt_str_t item;
	stream.text = *field;
	apt_text_stream_reset(&stream);

	/* read name */
	if(apt_text_field_read(&stream,'=',TRUE,&item) == FALSE) {
		return FALSE;
	}
	apt_string_copy(&pair->name,&item,pool);

	/* read value */
	apt_text_field_read(&stream,';',TRUE,&item);
	apt_string_copy(&pair->value,&item,pool);
	return TRUE;
}

/** Parse array of name-value pairs */
APT_DECLARE(apt_bool_t) apt_pair_array_parse(apt_pair_arr_t *arr, const apt_str_t *value, apr_pool_t *pool)
{
	apt_str_t field;
	apt_pair_t *pair;
	apt_text_stream_t stream;
	if(!arr || !value) {
		return FALSE;
	}

	stream.text = *value;
	apt_text_stream_reset(&stream);
	/* read name-value pairs */
	while(apt_text_field_read(&stream,';',TRUE,&field) == TRUE) {
		pair = apr_array_push(arr);
		apt_pair_parse(pair,&field,pool);
	}
	return TRUE;
}

/** Generate array of name-value pairs */
APT_DECLARE(apt_bool_t) apt_pair_array_generate(const apt_pair_arr_t *arr, apt_str_t *str, apr_pool_t *pool)
{
	int p, v = 0;
	struct iovec vec[512];
	const apt_pair_t *pair;
	static const int MAX_VECS = sizeof(vec) / sizeof(*vec);
	static const struct iovec IOV_SEMICOLON = {";", 1};
	static const struct iovec IOV_EQUALS = {"=", 1};

	for (p = 0; p < arr->nelts; p++) {
		pair = (apt_pair_t*)arr->elts + p;
		if (!pair->name.length)
			continue;
		if (v) {
			if (v >= MAX_VECS)
				return FALSE;
			vec[v++] = IOV_SEMICOLON;
		}
		if (v + (pair->value.length ? 3 : 1) > MAX_VECS)
			return FALSE;
		vec[v++] = *((struct iovec*)&pair->name);
		if (pair->value.length) {
			vec[v++] = IOV_EQUALS;
			vec[v++] = *((struct iovec*)&pair->value);
		}
	}
	str->buf = apr_pstrcatv(pool, vec, v, &str->length);
	return str->buf ? TRUE : FALSE;
}


/** Insert array of name-value pairs */
APT_DECLARE(apt_bool_t) apt_text_pair_array_insert(apt_text_stream_t *stream, const apt_pair_arr_t *arr)
{
	int i;
	apt_pair_t *pair;
	char *pos = stream->pos;
	if(!arr) {
		return FALSE;
	}

	for(i=0; i<arr->nelts; i++) {
		pair = (apt_pair_t*)arr->elts + i;
		if(i != 0) {
			if (pos >= stream->end)
				return FALSE;
			*pos++ = ';';
		}
		if(pair->name.length) {
			if (pos + pair->name.length +
				(pair->value.length ? pair->value.length + 1 : 0) > stream->end)
			{
				return FALSE;
			}
			memcpy(pos,pair->name.buf,pair->name.length);
			pos += pair->name.length;
			if(pair->value.length) {
				*pos++ = '=';
				memcpy(pos,pair->value.buf,pair->value.length);
				pos += pair->value.length;
			}
		}
	}
	stream->pos = pos;
	return TRUE;
}

/** Parse boolean-value */
APT_DECLARE(apt_bool_t) apt_boolean_value_parse(const apt_str_t *str, apt_bool_t *value)
{
	if(!str->buf) {
		return FALSE;
	}
	if(strncasecmp(str->buf,TOKEN_TRUE,TOKEN_TRUE_LENGTH) == 0) {
		*value = TRUE;
		return TRUE;
	}
	if(strncasecmp(str->buf,TOKEN_FALSE,TOKEN_FALSE_LENGTH) == 0) {
		*value = FALSE;
		return TRUE;
	}
	return FALSE;
}

/** Generate apr_size_t value from pool (buffer is allocated from pool) */
APT_DECLARE(apt_bool_t) apt_boolean_value_generate(apt_bool_t value, apt_str_t *str, apr_pool_t *pool)
{
	if(value == TRUE) {
		str->length = TOKEN_TRUE_LENGTH;
		str->buf = apr_palloc(pool,str->length);
		memcpy(str->buf,TOKEN_TRUE,str->length);
	}
	else {
		str->length = TOKEN_FALSE_LENGTH;
		str->buf = apr_palloc(pool,str->length);
		memcpy(str->buf,TOKEN_FALSE,str->length);
	}
	return TRUE;
}

/** Generate boolean-value */
APT_DECLARE(apt_bool_t) apt_boolean_value_insert(apt_text_stream_t *stream, apt_bool_t value)
{
	if(value == TRUE) {
		if(stream->pos + TOKEN_TRUE_LENGTH >= stream->end) {
			return FALSE;
		}
		memcpy(stream->pos,TOKEN_TRUE,TOKEN_TRUE_LENGTH);
		stream->pos += TOKEN_TRUE_LENGTH;
	}
	else {
		if(stream->pos + TOKEN_FALSE_LENGTH >= stream->end) {
			return FALSE;
		}
		memcpy(stream->pos,TOKEN_FALSE,TOKEN_FALSE_LENGTH);
		stream->pos += TOKEN_FALSE_LENGTH;
	}
	return TRUE;
}


/** Parse size_t value */
APT_DECLARE(apr_size_t) apt_size_value_parse(const apt_str_t *str)
{
	return str->buf ? atol(str->buf) : 0;
}

/** Generate apr_size_t value (buffer is allocated from pool) */
APT_DECLARE(apt_bool_t) apt_size_value_generate(apr_size_t value, apt_str_t *str, apr_pool_t *pool)
{
	str->buf = apr_psprintf(pool, "%"APR_SIZE_T_FMT, value);
	str->length = strlen(str->buf);
	return TRUE;
}

/** Insert apr_size_t value */
APT_DECLARE(apt_bool_t) apt_text_size_value_insert(apt_text_stream_t *stream, apr_size_t value)
{
	int length = apr_snprintf(stream->pos, stream->end - stream->pos, "%"APR_SIZE_T_FMT, value);
	if(length <= 0) {
		return FALSE;
	}
	stream->pos += length;
	return TRUE;
}


/** Parse float value */
APT_DECLARE(float) apt_float_value_parse(const apt_str_t *str)
{
	return str->buf ? (float)atof(str->buf) : 0;
}

/** Generate float value (buffer is allocated from pool) */
APT_DECLARE(apt_bool_t) apt_float_value_generate(float value, apt_str_t *str, apr_pool_t *pool)
{
	char *end;
	str->buf = apr_psprintf(pool,"%f",value);
	str->length = strlen(str->buf);

	/* remove trailing 0s (if any) */
	end = str->buf + str->length - 1;
	while(*end == 0x30 && end != str->buf && *(end - 1) != '.') end--;

	str->length = end - str->buf + 1;
	return TRUE;
}

/** Generate float value */
APT_DECLARE(apt_bool_t) apt_text_float_value_insert(apt_text_stream_t *stream, float value)
{
	char *end;
	int length = apr_snprintf(stream->pos, stream->end - stream->pos, "%f", value);
	if(length <= 0) {
		return FALSE;
	}

	/* remove trailing 0s (if any) */
	end = stream->pos + length - 1;
	while(*end == 0x30 && end != stream->pos && *(end - 1) != '.') end--;

	stream->pos = end + 1;
	return TRUE;
}

/** Insert string value */
APT_DECLARE(apt_bool_t) apt_text_string_insert(apt_text_stream_t *stream, const apt_str_t *str)
{
	if(stream->pos + str->length >= stream->end) {
		return FALSE;
	}
	if(str->length) {
		memcpy(stream->pos,str->buf,str->length);
		stream->pos += str->length;
	}
	return TRUE;
}

/** Generate value plus the length (number of digits) of the value itself. */
APT_DECLARE(apt_bool_t) apt_var_length_value_generate(apr_size_t *value, apr_size_t max_count, apt_str_t *str)
{
	/* (N >= (10^M-M)) ? N+M+1 : N+M */
	apr_size_t temp;
	apr_size_t count; /* M */
	apr_size_t bounds; /* 10^M */
	int length;

	/* calculate count */
	temp = *value;
	count = 0;
	do{count++; temp /= 10;} while(temp);

	/* calculate bounds */
	temp = count;
	bounds = 1;
	do{bounds *= 10; temp--;} while(temp);

	if(*value >= bounds - count) {
		count++;
	}

	*value += count;
	if(count > max_count) {
		return FALSE;
	}

	str->length = 0;
	length = sprintf(str->buf, "%"APR_SIZE_T_FMT, *value);
	if(length <= 0) {
		return FALSE;
	}
	str->length = length;
	return TRUE;
}

/** Generate completion-cause */
APT_DECLARE(apt_bool_t) apt_completion_cause_generate(const apt_str_table_item_t table[], apr_size_t size, apr_size_t cause, apt_str_t *str, apr_pool_t *pool)
{
	char buf[256];
	int length;
	const apt_str_t *name = apt_string_table_str_get(table,size,cause);
	if(!name) {
		return FALSE;
	}
	length = sprintf(buf,"%03"APR_SIZE_T_FMT" ",cause);
	if(length <= 0) {
		return FALSE;
	}

	memcpy(buf+length,name->buf,name->length);
	apt_string_assign_n(str,buf,name->length + length,pool);
	return TRUE;
}


/** Generate unique identifier (hex string) */
APT_DECLARE(apt_bool_t) apt_unique_id_generate(apt_str_t *id, apr_size_t length, apr_pool_t *pool)
{
	char *hex_str;
	apr_size_t i;
	apr_size_t count;
	apr_uuid_t uuid;
	apr_uuid_get(&uuid);
	
	hex_str = apr_palloc(pool,length+1);
	
	count = length / 2;
	if(count > sizeof(uuid)) {
		count = sizeof(uuid);
	}
	for(i=0; i<count; i++) {
		sprintf(hex_str+i*2,"%02x",uuid.data[i]);
	}
	hex_str[length] = '\0';

	id->buf = hex_str;
	id->length = length;
	return TRUE;
}
