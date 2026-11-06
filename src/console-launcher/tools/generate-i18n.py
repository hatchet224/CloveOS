#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate and compile the UTF-8 translation catalog. No network needed."""
from pathlib import Path
import json,re
root=Path(__file__).resolve().parents[1]
catalog=json.loads((root/'locales/es.json').read_text(encoding='utf-8'))
conversion=re.compile(r'%(?:%|[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|ll|[hljztL])?[diouxXeEfFgGaAcspn])')
for key,value in catalog.items():
    assert isinstance(key,str) and isinstance(value,str) and key and value
    assert '\x00' not in key+value
    assert conversion.findall(key)==conversion.findall(value),(key,value,'format mismatch')
header='''/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Generated from locales/es.json by tools/generate-i18n.py. */
#include "launcher/i18n.h"
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
static atomic_uint language = ATOMIC_VAR_INIT(CL_ENGLISH);
struct translation { const char *en, *es; };
static const struct translation translations[] = {
'''
tail='''};
void cl_language_set(unsigned int value)
{ atomic_store(&language,value == CL_SPANISH ? CL_SPANISH : CL_ENGLISH); }
enum cl_language cl_language_get(void)
{ return (enum cl_language)atomic_load(&language); }
const char *cl_tr(const char *english)
{
    if (english == NULL || cl_language_get() != CL_SPANISH) return english;
    size_t low=0U, high=sizeof(translations)/sizeof(translations[0]);
    while (low<high) {
        size_t middle=low+(high-low)/2U;
        int order=strcmp(english,translations[middle].en);
        if(order==0) return translations[middle].es;
        if(order<0) high=middle; else low=middle+1U;
    }
    return english;
}
void cl_copy_utf8(char *out,size_t capacity,const char *text)
{
    if(capacity==0U)return;
    size_t size=strlen(text);
    if(size>=capacity){size=capacity-1U;while(size>0U && ((unsigned char)text[size]&0xc0U)==0x80U)--size;}
    memcpy(out,text,size);out[size]='\\0';
}
'''
body=''.join('    {'+json.dumps(key,ensure_ascii=False)+', '+json.dumps(value,ensure_ascii=False)+'},\n' for key,value in sorted(catalog.items()))
(root/'src/i18n.c').write_text(header+body+tail,encoding='utf-8',newline='\n')
print(f'{len(catalog)} translations validated; printf conversions preserved')
