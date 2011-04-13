/*==LICENSE==*

CyanWorlds.com Engine - MMOG client, server and tools
Copyright (C) 2011  Cyan Worlds, Inc.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

You can contact Cyan Worlds, Inc. by email legal@cyan.com
 or by snail mail at:
      Cyan Worlds, Inc.
      14617 N Newport Hwy
      Mead, WA   99021

*==LICENSE==*/
/*****************************************************************************
*
*   $/Plasma20/Sources/Plasma/NucleusLib/pnUtils/Private/pnUtBase64.h
*   
***/

#ifdef PLASMA20_SOURCES_PLASMA_NUCLEUSLIB_PNUTILS_PRIVATE_PNUTBASE64_H
#error "Header $/Plasma20/Sources/Plasma/NucleusLib/pnUtils/Private/pnUtBase64.h included more than once"
#endif
#define PLASMA20_SOURCES_PLASMA_NUCLEUSLIB_PNUTILS_PRIVATE_PNUTBASE64_H



/*****************************************************************************
*
*   Base64 Codec API
*
***/

const unsigned kBase64EncodeBlock    = 4;
const unsigned kBase64EncodeMultiple = 3;

inline unsigned Base64EncodeSize (unsigned srcChars) {
    return (srcChars + kBase64EncodeMultiple - 1) / kBase64EncodeMultiple
         * kBase64EncodeBlock;
}
unsigned Base64Encode (
    unsigned    srcChars,
    const byte  srcData[],
    unsigned    dstChars,
    char *      dstData
);

inline unsigned Base64DecodeSize (unsigned srcChars, const char srcData[]) {
    return srcChars * kBase64EncodeMultiple / kBase64EncodeBlock
         - ((srcChars >= 1 && srcData[srcChars - 1] == '=') ? 1 : 0)
         - ((srcChars >= 2 && srcData[srcChars - 2] == '=') ? 1 : 0);
}
unsigned Base64Decode (
    unsigned    srcChars,
    const char  srcData[],
    unsigned    dstChars,
    byte *      dstData
);
