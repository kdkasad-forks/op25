/* -*- c++ -*- */
/* 
 * Copyright 2010, 2011, 2012, 2013, 2014 Max H. Parke KA1RBI 
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "p25p1_fdma.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <vector>
#include <sys/time.h>
#include "bch.h"
#include "op25_imbe_frame.h"
#include "p25_frame.h"
#include "p25_framer.h"
#include "rs.h"

namespace gr {
  namespace op25_repeater {

static const int64_t TIMEOUT_THRESHOLD = 1000000;

    p25p1_fdma::~p25p1_fdma()
    {
	delete framer;
    }

static uint16_t crc16(uint8_t buf[], int len) {
        uint32_t poly = (1<<12) + (1<<5) + (1<<0);
        uint32_t crc = 0;
        for(int i=0; i<len; i++) {
                uint8_t bits = buf[i];
                for (int j=0; j<8; j++) {
                        uint8_t bit = (bits >> (7-j)) & 1;
                        crc = ((crc << 1) | bit) & 0x1ffff;
                        if (crc & 0x10000)
                                crc = (crc & 0xffff) ^ poly;
		}
	}
        crc = crc ^ 0xffff;
        return crc & 0xffff;
}

/* translated from p25craft.py Michael Ossmann <mike@ossmann.com>  */
static uint32_t crc32(uint8_t buf[], int len) {	/* length is nr. of bits */
        uint32_t g = 0x04c11db7;
        uint64_t crc = 0;
        for (int i = 0; i < len; i++) {
                crc <<= 1;
                int b = ( buf [i / 8] >> (7 - (i % 8)) ) & 1;
                if (((crc >> 32) ^ b) & 1)
                        crc ^= g;
        }
        crc = (crc & 0xffffffff) ^ 0xffffffff;
        return crc;
}

/* find_min is from wireshark/plugins/p25/packet-p25cai.c */
/* Copyright 2008, Michael Ossmann <mike@ossmann.com>  */
/* return the index of the lowest value in a list */
static int
find_min(uint8_t list[], int len)
{
	int min = list[0];	
	int index = 0;	
	int unique = 1;	
	int i;

	for (i = 1; i < len; i++) {
		if (list[i] < min) {
			min = list[i];
			index = i;
			unique = 1;
		} else if (list[i] == min) {
			unique = 0;
		}
	}
	/* return -1 if a minimum can't be found */
	if (!unique)
		return -1;

	return index;
}

/* count_bits is from wireshark/plugins/p25/packet-p25cai.c */
/* Copyright 2008, Michael Ossmann <mike@ossmann.com>  */
/* count the number of 1 bits in an int */
static int
count_bits(unsigned int n)
{
    int i = 0;
    for (i = 0; n != 0; i++)
        n &= n - 1;
    return i;
}

/* adapted from wireshark/plugins/p25/packet-p25cai.c */
/* Copyright 2008, Michael Ossmann <mike@ossmann.com>  */
/* deinterleave and trellis1_2 decoding */
/* buf is assumed to be a buffer of 12 bytes */
static int
block_deinterleave(bit_vector& bv, unsigned int start, uint8_t* buf)
{
	static const uint16_t deinterleave_tb[] = {
	  0,  1,  2,  3,  52, 53, 54, 55, 100,101,102,103, 148,149,150,151,
	  4,  5,  6,  7,  56, 57, 58, 59, 104,105,106,107, 152,153,154,155,
	  8,  9, 10, 11,  60, 61, 62, 63, 108,109,110,111, 156,157,158,159,
	 12, 13, 14, 15,  64, 65, 66, 67, 112,113,114,115, 160,161,162,163,
	 16, 17, 18, 19,  68, 69, 70, 71, 116,117,118,119, 164,165,166,167,
	 20, 21, 22, 23,  72, 73, 74, 75, 120,121,122,123, 168,169,170,171,
	 24, 25, 26, 27,  76, 77, 78, 79, 124,125,126,127, 172,173,174,175,
	 28, 29, 30, 31,  80, 81, 82, 83, 128,129,130,131, 176,177,178,179,
	 32, 33, 34, 35,  84, 85, 86, 87, 132,133,134,135, 180,181,182,183,
	 36, 37, 38, 39,  88, 89, 90, 91, 136,137,138,139, 184,185,186,187,
	 40, 41, 42, 43,  92, 93, 94, 95, 140,141,142,143, 188,189,190,191,
	 44, 45, 46, 47,  96, 97, 98, 99, 144,145,146,147, 192,193,194,195,
	 48, 49, 50, 51 };

	uint8_t hd[4];
	int b, d, j;
	int state = 0;
	uint8_t codeword;
	uint16_t crc;
	uint32_t crc1;
	uint32_t crc2;

	static const uint8_t next_words[4][4] = {
		{0x2, 0xC, 0x1, 0xF},
		{0xE, 0x0, 0xD, 0x3},
		{0x9, 0x7, 0xA, 0x4},
		{0x5, 0xB, 0x6, 0x8}
	};

	memset(buf, 0, 12);

	for (b=0; b < 98*2; b += 4) {
		codeword = (bv[start+deinterleave_tb[b+0]] << 3) + 
		           (bv[start+deinterleave_tb[b+1]] << 2) + 
		           (bv[start+deinterleave_tb[b+2]] << 1) + 
		            bv[start+deinterleave_tb[b+3]]     ;

		/* try each codeword in a row of the state transition table */
		for (j = 0; j < 4; j++) {
			/* find Hamming distance for candidate */
			hd[j] = count_bits(codeword ^ next_words[state][j]);
		}
		/* find the dibit that matches the most codeword bits (minimum Hamming distance) */
		state = find_min(hd, 4);
		/* error if minimum can't be found */
		if(state == -1)
			return -1;	// decode error, return failure
		/* It also might be nice to report a condition where the minimum is
		 * non-zero, i.e. an error has been corrected.  It probably shouldn't
		 * be a permanent failure, though.
		 *
		 * DISSECTOR_ASSERT(hd[state] == 0);
		 */

		/* append dibit onto output buffer */
		d = b >> 2;	// dibit ctr
		if (d < 48) {
			buf[d >> 2] |= state << (6 - ((d%4) * 2));
		}
	}
	crc = crc16(buf, 12);
	if (crc == 0)
		return 0;	// return OK code
	crc1 = crc32(buf, 8*8);	// try crc32
	crc2 = (buf[8] << 24) + (buf[9] << 16) + (buf[10] << 8) + buf[11];
	if (crc1 == crc2)
		return 0;	// return OK code
	return -2;	// trellis decode OK, but CRC error occurred
}

p25p1_fdma::p25p1_fdma(const op25_udp& udp, int debug, bool do_imbe, bool do_output, bool do_msgq, gr::msg_queue::sptr queue, std::deque<int16_t> &output_queue, bool do_audio_output, bool do_nocrypt) :
        op25udp(udp),
	write_bufp(0),
	d_debug(debug),
	d_do_imbe(do_imbe),
	d_do_output(do_output),
	d_do_msgq(do_msgq),
	d_msg_queue(queue),
	output_queue(output_queue),
	framer(new p25_framer()),
        d_do_nocrypt(do_nocrypt),
	d_do_audio_output(do_audio_output),
        ess_algid(0),
        ess_keyid(0),
	vf_tgid(0),
	p1voice_decode((debug > 0), udp, output_queue)
{
	gettimeofday(&last_qtime, 0);
}

void 
p25p1_fdma::process_duid(uint32_t const duid, uint32_t const nac, uint8_t const buf[], int const len)
{
	char wbuf[256];
	int p = 0;
	if (!d_do_msgq)
		return;
	if (d_msg_queue->full_p())
		return;
	assert (len+2 <= sizeof(wbuf));
	wbuf[p++] = (nac >> 8) & 0xff;
	wbuf[p++] = nac & 0xff;
	if (buf) {
		memcpy(&wbuf[p], buf, len);	// copy data
		p += len;
	}
	gr::message::sptr msg = gr::message::make_from_string(std::string(wbuf, p), duid, 0, 0);
	d_msg_queue->insert_tail(msg);
	gettimeofday(&last_qtime, 0);
}

void
p25p1_fdma::process_HDU(const bit_vector& A)
{
        uint32_t MFID;
	int i, j, k, ec;
	std::vector<uint8_t> HB(63,0); // hexbit vector
	k = 0;
	for (i = 0; i < 36; i ++) {
		uint32_t CW = 0;
		for (j = 0; j < 18; j++) {  // 18 bits / cw
			CW = (CW << 1) + A [ hdu_codeword_bits[k++] ];
 		}
		HB[27 + i] = gly24128Dec(CW) & 63;
	}
	ec = rs16.decode(HB); // Reed Solomon (36,20,17) error correction

	if (ec < 0)
		return; // discard info if uncorrectable errors

	j = 27;												// 72 bit MI
	for (i = 0; i < 9;) {
		ess_mi[i++] = (uint8_t)  (HB[j  ]         << 2) + (HB[j+1] >> 4);
		ess_mi[i++] = (uint8_t) ((HB[j+1] & 0x0f) << 4) + (HB[j+2] >> 2);
		ess_mi[i++] = (uint8_t) ((HB[j+2] & 0x03) << 6) +  HB[j+3];
		j += 4;
	}
	MFID      =  (HB[j  ]         <<  2) + (HB[j+1] >> 4);						// 8 bit MfrId
	ess_algid = ((HB[j+1] & 0x0f) <<  4) + (HB[j+2] >> 2);						// 8 bit AlgId
	ess_keyid = ((HB[j+2] & 0x03) << 14) + (HB[j+3] << 8) + (HB[j+4] << 2) + (HB[j+5] >> 4);	// 16 bit KeyId
	vf_tgid   = ((HB[j+5] & 0x0f) << 12) + (HB[j+6] << 6) +  HB[j+7];				// 16 bit TGID

	if (d_debug >= 10) {
		fprintf (stderr, "HDU:  ESS: rs=%d, tgid=%d, mfid=%x, algid=%x, keyid=%x, mi=", ec, vf_tgid, MFID, ess_algid, ess_keyid);
		for (i = 0; i < 9; i++) {
			fprintf(stderr, "%02x ", ess_mi[i]);
		}
	}
}

void
p25p1_fdma::process_LLDU(const bit_vector& A, std::vector<uint8_t>& HB)
{
	process_duid(framer->duid, framer->nac, NULL, 0);

	int i, j, k;
	k = 0;
	for (i = 0; i < 24; i ++) { // 24 10-bit codewords
		uint32_t CW = 0;
		for (j = 0; j < 10; j++) {  // 10 bits / cw
			CW = (CW << 1) + A[ imbe_ldu_ls_data_bits[k++] ];
		}
		HB[39 + i] = hmg1063Dec( CW >> 4, CW & 0x0f );
	}
}

void
p25p1_fdma::process_LDU1(const bit_vector& A)
{
	std::vector<uint8_t> HB(63,0); // hexbit vector
	process_LLDU(A, HB);

	if (d_debug >= 10) {
		fprintf (stderr, "LDU1: ");
	}

	process_LCW(HB);
	process_voice(A);
}

void
p25p1_fdma::process_LDU2(const bit_vector& A)
{
	std::vector<uint8_t> HB(63,0); // hexbit vector
	process_LLDU(A, HB);

        int i, j, ec;
	ec = rs8.decode(HB); // Reed Solomon (24,16,9) error correction
	j = 39;												// 72 bit MI
	for (i = 0; i < 9;) {
		ess_mi[i++] = (uint8_t)  (HB[j  ]         << 2) + (HB[j+1] >> 4);
		ess_mi[i++] = (uint8_t) ((HB[j+1] & 0x0f) << 4) + (HB[j+2] >> 2);
		ess_mi[i++] = (uint8_t) ((HB[j+2] & 0x03) << 6) +  HB[j+3];
		j += 4;
	}

	if (ec >= 0) {	// save info if good decode
		ess_algid =  (HB[j  ]         <<  2) + (HB[j+1] >> 4);					// 8 bit AlgId
		ess_keyid = ((HB[j+1] & 0x0f) << 12) + (HB[j+2] << 6) + HB[j+3];			// 16 bit KeyId
	}

	if (d_debug >= 10) {
		fprintf(stderr, "LDU2: ESS: rs=%d, algid=%x, keyid=%x, mi=", ec, ess_algid, ess_keyid);
		for (int i = 0; i < 9; i++) {
			fprintf(stderr, "%02x ", ess_mi[i]);
		}
	}

	process_voice(A);
}

void
p25p1_fdma::process_TDU()
{
	process_duid(framer->duid, framer->nac, NULL, 0);

	if (d_debug >= 10) {
		fprintf (stderr, "TDU:  ");
	}

	if ((d_do_imbe || d_do_audio_output) && (framer->duid == 0x3 || framer->duid == 0xf)) {  // voice termination
		op25udp.send_audio_flag(op25_udp::DRAIN);
	}
}

void
p25p1_fdma::process_TDU(const bit_vector& A)
{
	process_TDU();

	int i, j, k;
	std::vector<uint8_t> HB(63,0); // hexbit vector
	k = 0;
	for (i = 0; i <= 22; i += 2) {
		uint32_t CW = 0;
		for (j = 0; j < 12; j++) {   // 12 24-bit codewords
			CW = (CW << 1) + A [ hdu_codeword_bits[k++] ];
			CW = (CW << 1) + A [ hdu_codeword_bits[k++] ];
		}
		uint32_t D = gly24128Dec(CW);
		HB[39 + i] = D >> 6;
		HB[40 + i] = D & 63;
	}
	process_LCW(HB);
}

void
p25p1_fdma::process_LCW(std::vector<uint8_t>& HB)
{
	int ec = rs12.decode(HB); // Reed Solomon (24,12,13) error correction
	int pb =   (HB[39] >> 5);
	int sf =  ((HB[39] & 0x10) >> 4);
	int lco = ((HB[39] & 0x0f) << 2) + (HB[40] >> 4);
	if (d_debug >= 10) {
		fprintf(stderr, "LCW: rs=%d, pb=%d, sf=%d, lco=%d", ec, pb, sf, lco);
	}
}

void
p25p1_fdma::process_TSBK(const bit_vector& fr, uint32_t fr_len)
{
	if (d_debug >= 10) {
		fprintf (stderr, "TSBK: ");
	}

	uint8_t deinterleave_buf[3][12];
	if (process_blocks(fr, fr_len, deinterleave_buf) != -1) {
		uint8_t op = deinterleave_buf[0][0] & 0x3f;
		process_duid(framer->duid, framer->nac, deinterleave_buf[0], 10);

		if (d_debug >= 10) {
			fprintf (stderr, "op=%02x : ", op);
			for (int i = 0; i < 10; i++) {
				fprintf(stderr, "%02x ", deinterleave_buf[0][i]);
			}
		}
	}
}

void
p25p1_fdma::process_PDU(const bit_vector& fr, uint32_t fr_len)
{
	if (d_debug >= 10) {
		fprintf (stderr, "PDU:  ");
	}

	uint8_t fmt, blks, op;
	uint8_t deinterleave_buf[3][12];
	int rc = process_blocks(fr, fr_len, deinterleave_buf);
	fmt =  deinterleave_buf[0][0] & 0x1f;
	blks = deinterleave_buf[0][6] & 0x7f;
	op =   deinterleave_buf[0][7] & 0x3f;

	if (d_debug >= 10) {
		fprintf (stderr, "rc=%d, fmt=%02x, blks=%d, op=%02x : \n", rc, fmt, blks+1, op);
		for (int j = 0; j < blks+1; j++) {
			for (int i = 0; i < 12; i++) {
				fprintf(stderr, "%02x ", deinterleave_buf[j][i]);
			}
		}
	}

	if ((rc == 0) && (fmt == 0x17)) {
		// Alternate Multi Block Trunking (MBT) message
		// we copy first 10 bytes header block and 
		// first 8 from last block (removes 2 byte and 4-byte CRC's)
		int blksz = (blks < 1) ? 10 : (((blks+1) * 12) - 6);
		uint8_t mbt_block[blksz];
		memcpy(&mbt_block[ 0], deinterleave_buf[0], 10);
		if (blks == 1) {
			memcpy(&mbt_block[10], deinterleave_buf[1], 8);
		} else if (blks == 2) {
			memcpy(&mbt_block[10], deinterleave_buf[1], 12);
			memcpy(&mbt_block[22], deinterleave_buf[2], 8);
		}
		process_duid(framer->duid, framer->nac, mbt_block, blksz);
	}
}

int
p25p1_fdma::process_blocks(const bit_vector& fr, uint32_t& fr_len, uint8_t (&dbuf)[3][12])
{
	int rc, blk_status = 0;
	unsigned int d, b;
	int sizes[3] = {360, 576, 720};
	bit_vector bv(720,0);
	
	if (fr_len > 720) {
		fprintf(stderr, "warning trunk frame size %u exceeds maximum\n", fr_len);
		fr_len = 720;
	}
	for (d=0, b=0; d < fr_len >> 1; d++) {	// eliminate status bits from frame
		if ((d+1) % 36 == 0)
			continue;
		bv[b++] = fr[d*2];
		bv[b++] = fr[d*2+1];
	}
	for(int sz=0; sz < 3; sz++) {
		if (fr_len >= sizes[sz]) {
			// deinterleave,  decode trellis1_2
			rc = block_deinterleave(bv, 48+64+sz*196, dbuf[sz]);
			if (rc == -1) { 
				blk_status = -1;
			}
		}
	}
	return blk_status;
}

void
p25p1_fdma::process_voice(const bit_vector& A)
{
	if (d_do_imbe || d_do_audio_output) {
		for(size_t i = 0; i < nof_voice_codewords; ++i) {
			voice_codeword cw(voice_codeword_sz);
			uint32_t E0, ET;
			uint32_t u[8];
			char s[128];
			imbe_deinterleave(A, cw, i);
			// recover 88-bit IMBE voice code word
			imbe_header_decode(cw, u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], E0, ET);
			// output one 32-byte msg per 0.020 sec.
			// also, 32*9 = 288 byte pkts (for use via UDP)
			sprintf(s, "%03x %03x %03x %03x %03x %03x %03x %03x\n", u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7]);
			if (d_do_audio_output) {
				if (!d_do_nocrypt || !encrypted()) {
					p1voice_decode.rxframe(u);
				} else if (d_debug > 1) {
					fprintf(stderr, "p25p1_fdma: encrypted audio algid(%0x)\n", ess_algid);
				}
			}

			if (d_do_output && !d_do_audio_output) {
				for (size_t j=0; j < strlen(s); j++) {
					output_queue.push_back(s[j]);
				}
			}
		}
	}
}

void
p25p1_fdma::reset_timer()
{
  //update last_qtime with current time
  gettimeofday(&last_qtime, 0);
}

void 
p25p1_fdma::rx_sym (const uint8_t *syms, int nsyms)
{
  struct timeval currtime;
  for (int i1 = 0; i1 < nsyms; i1++){
  	if(framer->rx_sym(syms[i1])) {   // complete frame was detected

		if (framer->nac == 0) {  // discard frame if NAC is invalid
			return;
		}

		if (d_debug >= 10) {
			fprintf (stderr, "NAC 0x%x, errs=%2u, ", framer->nac, framer->bch_errors);
		}

		// extract additional signalling information and voice codewords
		switch(framer->duid) {
			case 0x00:
				process_HDU(framer->frame_body);
				break;
			case 0x03:
				process_TDU();
				break;
			case 0x05:
				process_LDU1(framer->frame_body);
				break;
			case 0x07:
				process_TSBK(framer->frame_body, framer->frame_size);
				break;
			case 0x0a:
				process_LDU2(framer->frame_body);
				break;
			case 0x0c:
				process_PDU(framer->frame_body, framer->frame_size);
				break;
			case 0x0f:
				process_TDU(framer->frame_body);
				break;
		}
		if (d_debug >= 10) {
			fprintf(stderr,"\n");
		}

		if (!d_do_imbe) { // send raw frame to wireshark
			// pack the bits into bytes, MSB first
			size_t obuf_ct = 0;
			uint8_t obuf[P25_VOICE_FRAME_SIZE/2];
			for (uint32_t i = 0; i < framer->frame_size; i += 8) {
				uint8_t b = 
					(framer->frame_body[i+0] << 7) +
					(framer->frame_body[i+1] << 6) +
					(framer->frame_body[i+2] << 5) +
					(framer->frame_body[i+3] << 4) +
					(framer->frame_body[i+4] << 3) +
					(framer->frame_body[i+5] << 2) +
					(framer->frame_body[i+6] << 1) +
					(framer->frame_body[i+7]     );
				obuf[obuf_ct++] = b;
			}
			op25udp.send_to(obuf, obuf_ct);

			if (d_do_output) {
				for (size_t j=0; j < obuf_ct; j++) {
					output_queue.push_back(obuf[j]);
				}
			}
		}
  	}  // end of complete frame
  }
  if (d_do_msgq && !d_msg_queue->full_p()) {
    // check for timeout
    gettimeofday(&currtime, 0);
    int64_t diff_usec = currtime.tv_usec - last_qtime.tv_usec;
    int64_t diff_sec  = currtime.tv_sec  - last_qtime.tv_sec ;
    if (diff_usec < 0) {
      diff_usec += 1000000;
      diff_sec  -= 1;
    }
    diff_usec += diff_sec * 1000000;
    if (diff_usec >= TIMEOUT_THRESHOLD) {
      if (d_debug >= 10)
        fprintf(stderr, "%010lu.%06lu p25p1_fdma::rx_sym() timeout\n", currtime.tv_sec, currtime.tv_usec);

      if (d_do_audio_output) {
        op25udp.send_audio_flag(op25_udp::DRAIN);
      }

      gettimeofday(&last_qtime, 0);
      gr::message::sptr msg = gr::message::make(-1, 0, 0);
      d_msg_queue->insert_tail(msg);
    }
  }
}

  }  // namespace
}  // namespace
