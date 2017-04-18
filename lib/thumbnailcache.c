/*
 * This file is part of pqiv
 * Copyright (c) 2017, Phillip Berndt
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * This implements thumbnail caching as specified in
 * https://specifications.freedesktop.org/thumbnail-spec/thumbnail-spec-latest.html
 */

#ifndef CONFIGURED_WITHOUT_MONTAGE_MODE

#include "thumbnailcache.h"

#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <math.h>
#include <unistd.h>
#include <string.h>

#include <glib/gstdio.h>
#include <gio/gio.h>
#include <cairo.h>

static const char * const thumbnail_levels[] = { "large", "normal" };

/* CRC calculation as per PNG TR, Annex D */
static unsigned long crc_table[256];
static gboolean crc_table_computed = 0;
static void make_crc_table(void) {
	unsigned long c;
	int n, k;

	for(n = 0; n < 256; n++) {
		c = (unsigned long) n;
		for(k = 0; k < 8; k++) {
			if(c & 1) {
				c = 0xedb88320L ^ (c >> 1);
			}
			else {
				c >>= 1;
			}
		}
		crc_table[n] = c;
	}
	crc_table_computed = 1;
}
static unsigned long crc(unsigned long crc, unsigned char *buf, int len) {
	unsigned long c = crc ^ 0xffffffffL;
	int n;

	if (!crc_table_computed) {
		make_crc_table();
	}
	for (n = 0; n < len; n++) {
		c = crc_table[(c ^ buf[n]) & 0xff] ^ (c >> 8);
	}
	return c ^ 0xffffffffL;
}

/* Auxiliary functions */
static gchar *get_local_filename(file_t *file) {
	// Memory files do not have a file name
	if(file->file_flags & FILE_FLAGS_MEMORY_IMAGE) {
		return NULL;
	}

	// Multi-page documents do not have an unambigous file name
	// Since the Thumbnail Managing Standard does not state how to format an
	// URI into e.g. an archive, do not cache such files.
	gchar *display_basename = g_strrstr(file->display_name, G_DIR_SEPARATOR_S);
	if(display_basename) {
		display_basename++;
	}
	else {
		display_basename = file->display_name;
	}
	gchar *filename_basename = g_strrstr(file->file_name, G_DIR_SEPARATOR_S);
	if(filename_basename) {
		filename_basename++;
	}
	else {
		filename_basename = file->file_name;
	}
	if(strcmp(filename_basename, display_basename) != 0) {
		return NULL;
	}

	// Retrieve file name
	GFile *gfile = gfile_for_commandline_arg(file->file_name);
	gchar *file_path = g_file_get_path(gfile);
	g_object_unref(gfile);
	return file_path;
}

static gchar *_local_thumbnail_cache_directory;
static const gchar *get_thumbnail_cache_directory() {
	if(!_local_thumbnail_cache_directory) {
		const gchar *cache_dir = g_getenv("XDG_CACHE_HOME");
		if(!cache_dir) {
			_local_thumbnail_cache_directory = g_build_filename(g_getenv("HOME"), ".cache", "thumbnails", NULL);
		}
		else {
			_local_thumbnail_cache_directory = g_build_filename(cache_dir, "thumbnails", NULL);
		}
		if(!g_file_test(_local_thumbnail_cache_directory, G_FILE_TEST_IS_DIR)) {
			g_mkdir_with_parents(_local_thumbnail_cache_directory, 0600);
		}
	}
	return _local_thumbnail_cache_directory;
}

gboolean check_png_attributes(gchar *file_name, gchar *file_uri, time_t file_mtime) {
	// Parse PNG headers and check whether the Thumb::URI and Thumb::MTime
	// headers are up to date.
	//
	// See below in png_writer for a rough explaination, or read the PNG TR
	// https://www.w3.org/TR/PNG/
	//
	gboolean file_uri_match = FALSE;
	gboolean file_mtime_match = FALSE;

	int fd = g_open(file_name, O_RDONLY);
	if(fd < 0) {
		return FALSE;
	}

	char header_buf[8];

	// File header
	if(read(fd, header_buf, 8) != 8) {
		g_close(fd, NULL);
		return FALSE;
	}
	const char expected_header[] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	if(memcmp(header_buf, expected_header, sizeof(expected_header)) != 0) {
		g_close(fd, NULL);
		return FALSE;
	}

	// Read all chunks until we have both matches
	while(1) {
		if(read(fd, header_buf, 8) != 8) {
			g_close(fd, NULL);
			return FALSE;
		}

		unsigned header_length = ntohl(*(int32_t *)header_buf);

		if(strncmp(&header_buf[4], "tEXt", 4) == 0) {
			// This is interesting. Read the whole contents first.
			char *data = g_malloc(header_length);
			if(read(fd, data, header_length) != header_length) {
				g_close(fd, NULL);
				return FALSE;
			}

			// Check against CRC
			if(read(fd, header_buf, 4) != 4) {
				g_close(fd, NULL);
				return FALSE;
			}
			unsigned file_crc = ntohl(*(uint32_t *)header_buf);
			unsigned actual_crc = crc(crc(0, (unsigned char*)"tEXt", 4), (unsigned char *)data, header_length);

			if(file_crc == actual_crc) {
				if(strcmp(data, "Thumb::URI") == 0) {
					file_uri_match = strncmp(&data[sizeof("Thumb::URI")], file_uri, strlen(file_uri)) == 0;
				}
				else if(strcmp(data, "Thumb::MTime") == 0) {
					gchar *file_mtime_str = g_strdup_printf("%lu", file_mtime);
					file_mtime_match = strncmp(&data[sizeof("Thumb::MTime")], file_mtime_str, strlen(file_mtime_str)) == 0;
					g_free(file_mtime_str);
				}

				if(file_uri_match && file_mtime_match) {
					g_close(fd, NULL);
					return TRUE;
				}
			}
		}
		else {
			// Skip header and its CRC
			lseek(fd, header_length + 4, SEEK_CUR);
		}
	}

	g_close(fd, NULL);
	return FALSE;
}

static cairo_surface_t *load_thumbnail(gchar *file_name, gchar *file_uri, time_t file_mtime, unsigned width, unsigned height) {
	// Check if the file is up to date
	if(!check_png_attributes(file_name, file_uri, file_mtime)) {
		return NULL;
	}

	cairo_surface_t *thumbnail = cairo_image_surface_create_from_png(file_name);
	if(cairo_surface_status(thumbnail) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(thumbnail);
		return NULL;
	}

	unsigned actual_width  = cairo_image_surface_get_width(thumbnail);
	unsigned actual_height = cairo_image_surface_get_height(thumbnail);

	if(actual_width == width || actual_height == height) {
		return thumbnail;
	}

	double scale_factor = fmin(1., fmin(actual_width * 1. / width, actual_height * 1. / height));
	unsigned target_width = width * scale_factor;
	unsigned target_height = height * scale_factor;

	#if CAIRO_VERSION >= CAIRO_VERSION_ENCODE(1, 12, 0)
		cairo_surface_t *target_thumbnail = cairo_surface_create_similar_image(thumbnail, CAIRO_FORMAT_ARGB32, target_width, target_height);
	#else
		cairo_surface_t *target_thumbnail = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, target_width, target_height);
	#endif

	cairo_t *cr = cairo_create(target_thumbnail);
	cairo_scale(cr, scale_factor, scale_factor);
	cairo_set_source_surface(cr, thumbnail, 0, 0);
	cairo_paint(cr);
	cairo_destroy(cr);

	cairo_surface_destroy(thumbnail);
	thumbnail = target_thumbnail;
	if(cairo_surface_status(thumbnail) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(thumbnail);
		return NULL;
	}

	return thumbnail;
}

/* This library's public API */
gboolean load_thumbnail_from_cache(file_t *file, unsigned width, unsigned height) {
	// We can only use the cache if the thumbnail is sufficiently small
	if(width > 256 || height > 256) {
		return FALSE;
	}

	// Obtain a local path to the file
	gchar *local_filename = get_local_filename(file);
	if(!local_filename) {
		return FALSE;
	}

	// Obtain modification timestamp
	struct stat file_stat;
	stat(local_filename, &file_stat);
	time_t file_mtime = file_stat.st_mtime;

	// Obtain the name of the candidate for the local thumbnail file
	gchar *file_uri = g_strdup_printf("file://%s", local_filename);
	gchar *md5_filename = g_compute_checksum_for_string(G_CHECKSUM_MD5, file_uri, -1);

	for(int j=(width > 128 || height > 128) ? 0 : 1; j<2; j++) {
		gchar *thumbnail_candidate = g_strdup_printf("%s%s%s%s%s.png", get_thumbnail_cache_directory(), G_DIR_SEPARATOR_S, thumbnail_levels[j], G_DIR_SEPARATOR_S, md5_filename);
		if(g_file_test(thumbnail_candidate, G_FILE_TEST_EXISTS)) {
			cairo_surface_t *thumbnail = load_thumbnail(thumbnail_candidate, file_uri, file_mtime, width, height);
			g_free(thumbnail_candidate);
			if(thumbnail != NULL) {
				file->thumbnail = thumbnail;
				g_free(file_uri);
				g_free(md5_filename);
				return TRUE;
			}
		}
		else {
			g_free(thumbnail_candidate);
		}
	}

	g_free(file_uri);
	g_free(md5_filename);

	// Check if a shared thumbnail directory exists and try to load from there
	gchar *file_dirname  = g_path_get_dirname(local_filename);
	gchar *shared_thumbnail_directory = g_build_filename(file_dirname, ".sh_thumbnails", NULL);
	g_free(file_dirname);
	if(g_file_test(shared_thumbnail_directory, G_FILE_TEST_IS_DIR)) {
		gchar *file_basename = g_path_get_basename(local_filename);
		gchar *md5_basename = g_compute_checksum_for_string(G_CHECKSUM_MD5, file_basename, -1);
		g_free(file_basename);

		for(int j=(width > 128 || height > 128) ? 0 : 1; j<2; j++) {
			gchar *thumbnail_candidate = g_strdup_printf("%s%s%s%s%s.png", shared_thumbnail_directory, G_DIR_SEPARATOR_S, thumbnail_levels[j], G_DIR_SEPARATOR_S, md5_basename);
			if(g_file_test(thumbnail_candidate, G_FILE_TEST_EXISTS)) {
				cairo_surface_t *thumbnail = load_thumbnail(thumbnail_candidate, file_basename, file_mtime, width, height);
				g_free(thumbnail_candidate);
				if(thumbnail != NULL) {
					file->thumbnail = thumbnail;
					g_free(md5_basename);
					g_free(shared_thumbnail_directory);
					return TRUE;
				}
			}
			else {
				g_free(thumbnail_candidate);
			}
		}

		g_free(md5_basename);
	}
	g_free(shared_thumbnail_directory);

	return FALSE;
}

struct png_writer_info {
	int output_file_fd;
	size_t bytes_written;
	gchar *Thumb_URI;
	gchar *Thumb_MTime;
};

static cairo_status_t png_writer(struct png_writer_info *info, const unsigned char *data, unsigned int length) {
	// This is actually quite simple: A PNG file always begins with the bytes
	// (137, 80, 78, 71, 13, 10, 26, 10), followed by chunks, which are
	// 4 bytes payload length, 4 bytes (ASCII) type, payload, and 4 bytes CRC
	// as defined above, taken over type & payload.
	// We want to inject a chunk of type tEXt, whose payload is key\0value,
	// after the IHDR header, which does always come first, is required, and
	// has fixed length 13.
	//
	const unsigned inject_pos = 8 /* header */ + (4 + 4 + 4 + 13) /* IHDR */;
	if(info->bytes_written < inject_pos && info->bytes_written + length >= inject_pos) {
		if(write(info->output_file_fd, data, inject_pos - info->bytes_written) != (int)inject_pos - (ptrdiff_t)info->bytes_written) {
			return CAIRO_STATUS_WRITE_ERROR;
		}
		data += inject_pos - info->bytes_written;
		length -= inject_pos - info->bytes_written;
		info->bytes_written = inject_pos;

		int uri_length = strlen(info->Thumb_URI);
		int output_length = 4 + 4 + sizeof("Thumb::URI") + uri_length + 4;
		char *output = g_malloc(output_length);
		*(uint32_t*)output = htonl(sizeof("Thumb::URI") + uri_length);
		strcpy(&output[4], "tEXtThumb::URI");
		strcpy(&output[19], info->Thumb_URI);
		*(uint32_t*)&output[19+uri_length] = htonl(crc(0, (unsigned char*)&output[4], 19 + uri_length - 4));
		if(write(info->output_file_fd, output, output_length) != output_length) {
			return CAIRO_STATUS_WRITE_ERROR;
		}
		g_free(output);

		int mtime_length = strlen(info->Thumb_MTime);
		output_length = 4 + 4 + sizeof("Thumb::MTime") + mtime_length + 4;
		output = g_malloc(output_length);
		*(uint32_t*)output = htonl(sizeof("Thumb::MTime") + mtime_length);
		strcpy(&output[4], "tEXtThumb::MTime");
		strcpy(&output[21], info->Thumb_MTime);
		*(uint32_t*)&output[21+mtime_length] = htonl(crc(0, (unsigned char*)&output[4], 21 + mtime_length - 4));
		if(write(info->output_file_fd, output, output_length) != output_length) {
			return CAIRO_STATUS_WRITE_ERROR;
		}
		g_free(output);
	}

	if(length > 0) {
		if(write(info->output_file_fd, data, length) != length) {
			return CAIRO_STATUS_WRITE_ERROR;
		}
		info->bytes_written += length;
	}

	return CAIRO_STATUS_SUCCESS;
}

gboolean store_thumbnail_to_cache(file_t *file) {
	// We only store thumbnails if they have the correct size
	unsigned width  = cairo_image_surface_get_width(file->thumbnail);
	unsigned height = cairo_image_surface_get_height(file->thumbnail);
	int thumbnail_level;

	if(width == 256 || height == 256) {
		thumbnail_level = 0;
	}
	else if(width == 128 || height == 128) {
		thumbnail_level = 1;
	}
	else {
		return FALSE;
	}

	// Obtain absolute path to file
	gchar *local_filename = get_local_filename(file);
	if(!local_filename) {
		return FALSE;
	}

	// Obtain modification timestamp
	struct stat file_stat;
	stat(local_filename, &file_stat);
	time_t file_mtime = file_stat.st_mtime;

	// Obtain the name of the thumbnail file
	gchar *file_uri = g_strdup_printf("file://%s", local_filename);
	gchar *md5_filename = g_compute_checksum_for_string(G_CHECKSUM_MD5, file_uri, -1);
	gchar *thumbnail_file = g_strdup_printf("%s%s%s%s%s.png", get_thumbnail_cache_directory(), G_DIR_SEPARATOR_S, thumbnail_levels[thumbnail_level], G_DIR_SEPARATOR_S, md5_filename);

	// Write out thumbnail
	// We use a wrapper to inject the tEXt chunks as required by the thumbnail standard
	gboolean retval = TRUE;
	int file_fd = g_open(thumbnail_file, O_CREAT | O_WRONLY, 0600);
	if(file_fd > 0) {
		gchar *string_mtime = g_strdup_printf("%lu", file_mtime);
		struct png_writer_info writer_info = { file_fd, 0, file_uri, string_mtime };
		if(cairo_surface_write_to_png_stream(file->thumbnail, (cairo_write_func_t)png_writer, &writer_info) != CAIRO_STATUS_SUCCESS) {
			g_unlink(thumbnail_file);
			retval = FALSE;
		}
		g_free(string_mtime);
	}
	g_close(file_fd, NULL);

	g_free(file_uri);
	g_free(md5_filename);
	g_free(thumbnail_file);

	return retval;
}

#endif
