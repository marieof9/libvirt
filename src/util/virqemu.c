/*
 * virqemu.c: utilities for working with qemu and its tools
 *
 * Copyright (C) 2016 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 */


#include <config.h>

#include "virbuffer.h"
#include "virerror.h"
#include "virlog.h"
#include "virqemu.h"

#define VIR_FROM_THIS VIR_FROM_NONE

VIR_LOG_INIT("util.qemu");


static int
virQEMUBuildCommandLineJSONRecurse(const char *key,
                                   const virJSONValue *value,
                                   virBufferPtr buf,
                                   bool nested)
{
    virJSONValuePtr elem;
    virBitmapPtr bitmap = NULL;
    ssize_t pos = -1;
    ssize_t end;
    size_t i;

    switch ((virJSONType) value->type) {
    case VIR_JSON_TYPE_STRING:
        virBufferAsprintf(buf, ",%s=", key);
        virQEMUBuildBufferEscapeComma(buf, value->data.string);
        break;

    case VIR_JSON_TYPE_NUMBER:
        virBufferAsprintf(buf, ",%s=%s", key, value->data.number);
        break;

    case VIR_JSON_TYPE_BOOLEAN:
        if (value->data.boolean)
            virBufferAsprintf(buf, ",%s=yes", key);
        else
            virBufferAsprintf(buf, ",%s=no", key);

        break;

    case VIR_JSON_TYPE_ARRAY:
        if (nested) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("nested JSON array to commandline conversion is "
                             "not supported"));
            return -1;
        }

        if (virJSONValueGetArrayAsBitmap(value, &bitmap) == 0) {
            while ((pos = virBitmapNextSetBit(bitmap, pos)) > -1) {
                if ((end = virBitmapNextClearBit(bitmap, pos)) < 0)
                    end = virBitmapLastSetBit(bitmap) + 1;

                if (end - 1 > pos) {
                    virBufferAsprintf(buf, ",%s=%zd-%zd", key, pos, end - 1);
                    pos = end;
                } else {
                    virBufferAsprintf(buf, ",%s=%zd", key, pos);
                }
            }
        } else {
            /* fallback, treat the array as a non-bitmap, adding the key
             * for each member */
            for (i = 0; i < virJSONValueArraySize(value); i++) {
                elem = virJSONValueArrayGet((virJSONValuePtr)value, i);

                /* recurse to avoid duplicating code */
                if (virQEMUBuildCommandLineJSONRecurse(key, elem, buf, true) < 0)
                    return -1;
            }
        }
        break;

    case VIR_JSON_TYPE_OBJECT:
    case VIR_JSON_TYPE_NULL:
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("NULL and OBJECT JSON types can't be converted to "
                         "commandline string"));
        return -1;
    }

    virBitmapFree(bitmap);
    return 0;
}


static int
virQEMUBuildCommandLineJSONIterate(const char *key,
                                   const virJSONValue *value,
                                   void *opaque)
{
    return virQEMUBuildCommandLineJSONRecurse(key, value, opaque, false);
}


/**
 * virQEMUBuildCommandLineJSON:
 * @value: json object containing the value
 * @buf: otuput buffer
 *
 * Formats JSON value object into command line parameters suitable for use with
 * qemu.
 *
 * Returns 0 on success -1 on error.
 */
int
virQEMUBuildCommandLineJSON(const virJSONValue *value,
                            virBufferPtr buf)
{
    if (virJSONValueObjectForeachKeyValue(value,
                                          virQEMUBuildCommandLineJSONIterate,
                                          buf) < 0)
        return -1;

    return 0;
}


char *
virQEMUBuildObjectCommandlineFromJSON(const char *type,
                                      const char *alias,
                                      virJSONValuePtr props)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char *ret = NULL;

    virBufferAsprintf(&buf, "%s,id=%s", type, alias);

    if (virQEMUBuildCommandLineJSON(props, &buf) < 0)
        goto cleanup;

    if (virBufferCheckError(&buf) < 0)
        goto cleanup;

    ret = virBufferContentAndReset(&buf);

 cleanup:
    virBufferFreeAndReset(&buf);
    return ret;
}


/**
 * virQEMUBuildBufferEscapeComma:
 * @buf: buffer to append the escaped string
 * @str: the string to escape
 *
 * qemu requires that any values passed on the command line which contain
 * a ',' must escape it using an extra ',' as the escape character
 */
void
virQEMUBuildBufferEscapeComma(virBufferPtr buf, const char *str)
{
    virBufferEscape(buf, ',', ",", "%s", str);
}


/**
 * virQEMUBuildLuksOpts:
 * @buf: buffer to build the string into
 * @enc: pointer to encryption info
 * @alias: alias to use
 *
 * Generate the string for id=$alias and any encryption options for
 * into the buffer.
 *
 * Important note, a trailing comma (",") is built into the return since
 * it's expected other arguments are appended after the id=$alias string.
 * So either turn something like:
 *
 *     "key-secret=$alias,"
 *
 * or
 *     "key-secret=$alias,cipher-alg=twofish-256,cipher-mode=cbc,
 *     hash-alg=sha256,ivgen-alg=plain64,igven-hash-alg=sha256,"
 *
 */
void
virQEMUBuildLuksOpts(virBufferPtr buf,
                     virStorageEncryptionInfoDefPtr enc,
                     const char *alias)
{
    virBufferAsprintf(buf, "key-secret=%s,", alias);

    if (!enc->cipher_name)
        return;

    virBufferAddLit(buf, "cipher-alg=");
    virQEMUBuildBufferEscapeComma(buf, enc->cipher_name);
    virBufferAsprintf(buf, "-%u,", enc->cipher_size);
    if (enc->cipher_mode) {
        virBufferAddLit(buf, "cipher-mode=");
        virQEMUBuildBufferEscapeComma(buf, enc->cipher_mode);
        virBufferAddLit(buf, ",");
    }
    if (enc->cipher_hash) {
        virBufferAddLit(buf, "hash-alg=");
        virQEMUBuildBufferEscapeComma(buf, enc->cipher_hash);
        virBufferAddLit(buf, ",");
    }
    if (!enc->ivgen_name)
        return;

    virBufferAddLit(buf, "ivgen-alg=");
    virQEMUBuildBufferEscapeComma(buf, enc->ivgen_name);
    virBufferAddLit(buf, ",");

    if (enc->ivgen_hash) {
        virBufferAddLit(buf, "ivgen-hash-alg=");
        virQEMUBuildBufferEscapeComma(buf, enc->ivgen_hash);
        virBufferAddLit(buf, ",");
    }
}
