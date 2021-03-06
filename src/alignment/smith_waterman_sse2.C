/******************************************************************
  Copyright 2006 by Michael Farrar.  All rights reserved.
  This program may not be sold or incorporated into a commercial product,
  in whole or in part, without written consent of Michael Farrar. 
 *******************************************************************/

/*
   Written by Michael Farrar, 2006 (alignment), Mengyao Zhao (SSW Library) and Martin Steinegger, 2015 (adapted it for MMseqs).
*/

#include "smith_waterman_sse2.h"
#include "../commons/Util.h"


#ifdef __GNUC__
#define LIKELY(x) __builtin_expect((x),1)
#define UNLIKELY(x) __builtin_expect((x),0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif



SmithWaterman::SmithWaterman(int maxSequenceLength, int aaSize) {
    const int segSize = (maxSequenceLength+7)/8;
    vHStore = (__m128i*) Util::mem_align(16,segSize * sizeof(__m128i));
	vHLoad  = (__m128i*) Util::mem_align(16,segSize * sizeof(__m128i));
	vE      = (__m128i*) Util::mem_align(16,segSize * sizeof(__m128i));
	vHmax   = (__m128i*) Util::mem_align(16,segSize * sizeof(__m128i));
    profile = new s_profile();
    profile->profile_byte = (__m128i*)Util::mem_align(16, aaSize * segSize * sizeof(__m128i));
    profile->profile_word = (__m128i*)Util::mem_align(16, aaSize * segSize * sizeof(__m128i));
    profile->profile_rev_byte = (__m128i*)Util::mem_align(16, aaSize * segSize * sizeof(__m128i));
    profile->profile_rev_word = (__m128i*)Util::mem_align(16, aaSize * segSize * sizeof(__m128i));
    profile->query_rev_sequence = new int8_t[maxSequenceLength];
    profile->query_sequence = new int8_t[maxSequenceLength];
    memset(profile->query_sequence, 0, maxSequenceLength*sizeof(int8_t));
    memset(profile->query_rev_sequence, 0, maxSequenceLength*sizeof(int8_t));
    
    /* array to record the largest score of each reference position */
	maxColumn = new uint8_t[maxSequenceLength*sizeof(uint16_t)];
    memset(maxColumn, 0, maxSequenceLength*sizeof(uint16_t));
    
}

SmithWaterman::~SmithWaterman(){
   	free(vHStore);
    free(vHmax);
	free(vE);
	free(vHLoad);
    free(profile->profile_byte);
    free(profile->profile_word);
    free(profile->profile_rev_byte);
    free(profile->profile_rev_word);
    
    delete [] profile->query_rev_sequence;
    delete [] profile->query_sequence;
    delete profile;
    delete [] maxColumn;
}

s_align* SmithWaterman::ssw_align (
                                   const int *db_sequence,
                                   int32_t db_length,
                                   const uint8_t gap_open,
                                   const uint8_t gap_extend,
                                   const uint8_t flag,	//  (from high to low) bit 5: return the best alignment beginning position; 6: if (ref_end1 - ref_begin1 <= filterd) && (read_end1 - read_begin1 <= filterd), return cigar; 7: if max score >= filters, return cigar; 8: always return cigar; if 6 & 7 are both setted, only return cigar when both filter fulfilled
                                   const uint16_t filters,
                                   const int32_t filterd,
                                   const int32_t maskLen) {
    
	alignment_end* bests = 0, *bests_reverse = 0;
	int32_t word = 0, band_width = 0, query_length = profile->query_length;
	cigar* path;
	s_align* r = new s_align;
	r->dbStartPos1 = -1;
	r->qStartPos1 = -1;
	r->cigar = 0;
	r->cigarLen = 0;
	//if (maskLen < 15) {
	//	fprintf(stderr, "When maskLen < 15, the function ssw_align doesn't return 2nd best alignment information.\n");
	//}
    
	// Find the alignment scores and ending positions
	if (profile->profile_byte) {
		bests = sw_sse2_byte(db_sequence, 0, db_length, query_length, gap_open, gap_extend, profile->profile_byte, -1, profile->bias, maskLen);

		if (profile->profile_word && bests[0].score == 255) {
			free(bests);
			bests = sw_sse2_word(db_sequence, 0, db_length, query_length, gap_open, gap_extend, profile->profile_word, -1, maskLen);
			word = 1;
		} else if (bests[0].score == 255) {
			fprintf(stderr, "Please set 2 to the score_size parameter of the function ssw_init, otherwise the alignment results will be incorrect.\n");
			delete r;
			return NULL;
		}
	}else if (profile->profile_word) {
		bests = sw_sse2_word(db_sequence, 0, db_length, query_length, gap_open, gap_extend, profile->profile_word, -1, maskLen);
		word = 1;
	}else {
		fprintf(stderr, "Please call the function ssw_init before ssw_align.\n");
		delete r;
		return NULL;
	}
	r->score1 = bests[0].score;
	r->dbEndPos1 = bests[0].ref;
	r->qEndPos1 = bests[0].read;
	if (maskLen >= 15) {
		r->score2 = bests[1].score;
		r->ref_end2 = bests[1].ref;
	} else {
		r->score2 = 0;
		r->ref_end2 = -1;
	}
	free(bests);
    int32_t queryOffset = query_length - r->qEndPos1;
    
	if (flag == 0 || (flag == 2 && r->score1 < filters)){
        goto end;
    }
    
	// Find the beginning position of the best alignment.
	if (word == 0) {
		createQueryProfile<int8_t,16>(profile->profile_rev_byte,
                                      profile->query_rev_sequence + queryOffset, //TODO offset them
                                      profile->mat, r->qEndPos1 + 1, profile->alphabetSize, profile->bias);
		bests_reverse = sw_sse2_byte(db_sequence, 1, r->dbEndPos1 + 1, r->qEndPos1 + 1, gap_open, gap_extend, profile->profile_rev_byte,
                                     r->score1, profile->bias, maskLen);
	} else {
		createQueryProfile<int16_t,8>(profile->profile_rev_word,
                                      profile->query_rev_sequence + queryOffset,
                                      profile->mat, r->qEndPos1 + 1, profile->alphabetSize, 0);
		bests_reverse = sw_sse2_word(db_sequence, 1, r->dbEndPos1 + 1, r->qEndPos1 + 1, gap_open, gap_extend, profile->profile_rev_word,
                                     r->score1, maskLen);
	}
    	if(bests_reverse->score != r->score1){
		fprintf(stderr, "Score of forward/backward SW differ. This should not happen.\n");
		delete r;
		return NULL;
	}

	r->dbStartPos1 = bests_reverse[0].ref;
	r->qStartPos1 = r->qEndPos1 - bests_reverse[0].read;
    
	free(bests_reverse);
	if ((7&flag) == 0 || ((2&flag) != 0 && r->score1 < filters) || ((4&flag) != 0
                                                                    && (r->dbEndPos1 - r->dbStartPos1 > filterd || r->qEndPos1 - r->qStartPos1 > filterd)))
        goto end;
    
	// Generate cigar.
	db_length = r->dbEndPos1 - r->dbStartPos1 + 1;
	query_length = r->qEndPos1 - r->qStartPos1 + 1;
	band_width = abs(db_length - query_length) + 1;
	path = banded_sw(db_sequence + r->dbStartPos1, profile->query_sequence + r->qStartPos1,
                     db_length, query_length, r->score1, gap_open, gap_extend,
                     band_width,
                     profile->mat,
                     profile->alphabetSize);
	if (path == 0) {
		delete r;
		r = NULL;
	}
	else {
		r->cigar = path->seq;
		r->cigarLen = path->length;
	}	delete(path);
    
    
end:
	return r;
}


/* Generate query profile rearrange query sequence & calculate the weight of match/mismatch. */
template <typename T, size_t Elements> void SmithWaterman::createQueryProfile (
                         __m128i* profile,
                         const int8_t* query_sequence,
                         const int8_t* mat,
                         const int32_t query_length,
                         const int32_t aaSize,	/* the edge length of the squre matrix mat */
                         uint8_t bias) {
    
    const int32_t segLen = (query_length+Elements-1)/Elements;
	T* t = (T*)profile;
    
	/* Generate query profile rearrange query sequence & calculate the weight of match/mismatch */
	for (int32_t nt = 0; LIKELY(nt < aaSize); nt ++) {
		for (int32_t i = 0; i < segLen; i ++) {
			int32_t  j = i;
			for (size_t segNum = 0; LIKELY(segNum < Elements) ; segNum ++) {
				*t++ = (j>= query_length) ? bias : mat[nt * aaSize + query_sequence[j]] + bias;
				j += segLen;
			}
        }
	}
}

static void seq_reverse(int8_t * reverse, const int8_t* seq, int32_t end)	/* end is 0-based alignment ending position */
{
	int32_t start = 0;
	while (LIKELY(start <= end)) {
		reverse[start] = seq[end];
		reverse[end] = seq[start];
		++start;
		--end;
	}
}


char SmithWaterman::cigar_int_to_op (uint32_t cigar_int)
{
	uint8_t letter_code = cigar_int & 0xfU;
	static const char map[] = {
		'M',
		'I',
		'D',
		'N',
		'S',
		'H',
		'P',
		'=',
		'X',
	};
    
	if (letter_code >= (sizeof(map)/sizeof(map[0]))) {
		return 'M';
	}
    
	return map[letter_code];
}

uint32_t SmithWaterman::cigar_int_to_len (uint32_t cigar_int)
{
	uint32_t res = cigar_int >> 4;
	return res;
}

SmithWaterman::alignment_end* SmithWaterman::sw_sse2_byte (const int* db_sequence,
                                    int8_t ref_dir,	// 0: forward ref; 1: reverse ref
                                    int32_t db_length,
                                    int32_t query_lenght,
                                    const uint8_t gap_open, /* will be used as - */
                                    const uint8_t gap_extend, /* will be used as - */
                                    const __m128i* query_profile_byte,
                                    uint8_t terminate,	/* the best alignment score: used to terminate
                                                         the matrix calculation when locating the
                                                         alignment beginning point. If this score
                                                         is set to 0, it will not be used */
                                    uint8_t bias,  /* Shift 0 point to a positive value. */
                                    int32_t maskLen) {
    
#define max16(m, vm) (vm) = _mm_max_epu8((vm), _mm_srli_si128((vm), 8)); \
(vm) = _mm_max_epu8((vm), _mm_srli_si128((vm), 4)); \
(vm) = _mm_max_epu8((vm), _mm_srli_si128((vm), 2)); \
(vm) = _mm_max_epu8((vm), _mm_srli_si128((vm), 1)); \
(m) = _mm_extract_epi16((vm), 0)
    
	uint8_t max = 0;		                     /* the max alignment score */
	int32_t end_read = query_lenght - 1;
	int32_t end_ref = -1; /* 0_based best alignment ending point; Initialized as isn't aligned -1. */
	int32_t segLen = (query_lenght + 15) / 16; /* number of segment */
	/* array to record the largest score of each reference position */
	memset(this->maxColumn, 0, db_length * sizeof(uint8_t));
    uint8_t * maxColumn = (uint8_t *) this->maxColumn;
    
	/* Define 16 byte 0 vector. */
	__m128i vZero = _mm_set1_epi32(0);
    __m128i* pvHStore = vHStore;
    __m128i* pvHLoad = vHLoad;
    __m128i* pvE = vE;
    __m128i* pvHmax = vHmax;
	memset(pvHStore,0,segLen*sizeof(__m128i));
    memset(pvHLoad,0,segLen*sizeof(__m128i));
    memset(pvE,0,segLen*sizeof(__m128i));
    memset(pvHmax,0,segLen*sizeof(__m128i));

	int32_t i, j;
	/* 16 byte insertion begin vector */
	__m128i vGapO = _mm_set1_epi8(gap_open);
    
	/* 16 byte insertion extension vector */
	__m128i vGapE = _mm_set1_epi8(gap_extend);
    
	/* 16 byte bias vector */
	__m128i vBias = _mm_set1_epi8(bias);
    
	__m128i vMaxScore = vZero; /* Trace the highest score of the whole SW matrix. */
	__m128i vMaxMark = vZero; /* Trace the highest score till the previous column. */
	__m128i vTemp;
	int32_t edge, begin = 0, end = db_length, step = 1;
    //	int32_t distance = query_lenght * 2 / 3;
    //	int32_t distance = query_lenght / 2;
    //	int32_t distance = query_lenght;
    
	/* outer loop to process the reference sequence */
	if (ref_dir == 1) {
		begin = db_length - 1;
		end = -1;
		step = -1;
	}
	for (i = begin; LIKELY(i != end); i += step) {
		int32_t cmp;
		__m128i e, vF = vZero, vMaxColumn = vZero; /* Initialize F value to 0.
                                                    Any errors to vH values will be corrected in the Lazy_F loop.
                                                    */
        //		max16(maxColumn[i], vMaxColumn);
        //		fprintf(stderr, "middle[%d]: %d\n", i, maxColumn[i]);
        
		__m128i vH = pvHStore[segLen - 1];
		vH = _mm_slli_si128 (vH, 1); /* Shift the 128-bit value in vH left by 1 byte. */
		const __m128i* vP = query_profile_byte + db_sequence[i] * segLen; /* Right part of the query_profile_byte */
        //	int8_t* t;
        //	int32_t ti;
        //        fprintf(stderr, "i: %d of %d:\t ", i,segLen);
        //for (t = (int8_t*)vP, ti = 0; ti < segLen; ++ti) fprintf(stderr, "%d\t", *t++);
        //fprintf(stderr, "\n");
        
        /* Swap the 2 H buffers. */
		__m128i* pv = pvHLoad;
		pvHLoad = pvHStore;
		pvHStore = pv;
        
		/* inner loop to process the query sequence */
		for (j = 0; LIKELY(j < segLen); ++j) {
			vH = _mm_adds_epu8(vH, _mm_load_si128(vP + j));
			vH = _mm_subs_epu8(vH, vBias); /* vH will be always > 0 */
            //	max16(maxColumn[i], vH);
            //	fprintf(stderr, "H[%d]: %d\n", i, maxColumn[i]);
            //	int8_t* t;
            //	int32_t ti;
            //for (t = (int8_t*)&vH, ti = 0; ti < 16; ++ti) fprintf(stderr, "%d\t", *t++);
            
			/* Get max from vH, vE and vF. */
			e = _mm_load_si128(pvE + j);
			vH = _mm_max_epu8(vH, e);
			vH = _mm_max_epu8(vH, vF);
			vMaxColumn = _mm_max_epu8(vMaxColumn, vH);
            
            //	max16(maxColumn[i], vMaxColumn);
            //	fprintf(stderr, "middle[%d]: %d\n", i, maxColumn[i]);
            //	for (t = (int8_t*)&vMaxColumn, ti = 0; ti < 16; ++ti) fprintf(stderr, "%d\t", *t++);
            
			/* Save vH values. */
			_mm_store_si128(pvHStore + j, vH);
            
			/* Update vE value. */
			vH = _mm_subs_epu8(vH, vGapO); /* saturation arithmetic, result >= 0 */
			e = _mm_subs_epu8(e, vGapE);
			e = _mm_max_epu8(e, vH);
			_mm_store_si128(pvE + j, e);
            
			/* Update vF value. */
			vF = _mm_subs_epu8(vF, vGapE);
			vF = _mm_max_epu8(vF, vH);
            
			/* Load the next vH. */
			vH = _mm_load_si128(pvHLoad + j);
		}
        
		/* Lazy_F loop: has been revised to disallow adjecent insertion and then deletion, so don't update E(i, j), learn from SWPS3 */
        /* reset pointers to the start of the saved data */
        j = 0;
        vH = _mm_load_si128 (pvHStore + j);
        
        /*  the computed vF value is for the given column.  since */
        /*  we are at the end, we need to shift the vF value over */
        /*  to the next column. */
        vF = _mm_slli_si128 (vF, 1);
        vTemp = _mm_subs_epu8 (vH, vGapO);
		vTemp = _mm_subs_epu8 (vF, vTemp);
		vTemp = _mm_cmpeq_epi8 (vTemp, vZero);
		cmp  = _mm_movemask_epi8 (vTemp);
        
        while (cmp != 0xffff)
        {
            vH = _mm_max_epu8 (vH, vF);
			vMaxColumn = _mm_max_epu8(vMaxColumn, vH);
            _mm_store_si128 (pvHStore + j, vH);
            vF = _mm_subs_epu8 (vF, vGapE);
            j++;
            if (j >= segLen)
            {
                j = 0;
                vF = _mm_slli_si128 (vF, 1);
            }
            vH = _mm_load_si128 (pvHStore + j);
            
            vTemp = _mm_subs_epu8 (vH, vGapO);
            vTemp = _mm_subs_epu8 (vF, vTemp);
            vTemp = _mm_cmpeq_epi8 (vTemp, vZero);
            cmp  = _mm_movemask_epi8 (vTemp);
        }
        
		vMaxScore = _mm_max_epu8(vMaxScore, vMaxColumn);
		vTemp = _mm_cmpeq_epi8(vMaxMark, vMaxScore);
		cmp = _mm_movemask_epi8(vTemp);
		if (cmp != 0xffff) {
			uint8_t temp;
			vMaxMark = vMaxScore;
			max16(temp, vMaxScore);
			vMaxScore = vMaxMark;
            
			if (LIKELY(temp > max)) {
				max = temp;
				if (max + bias >= 255) break;	//overflow
				end_ref = i;
                
				/* Store the column with the highest alignment score in order to trace the alignment ending position on read. */
				for (j = 0; LIKELY(j < segLen); ++j) pvHmax[j] = pvHStore[j];
			}
		}
        
		/* Record the max score of current column. */
		max16(maxColumn[i], vMaxColumn);
        //		fprintf(stderr, "maxColumn[%d]: %d\n", i, maxColumn[i]);
		if (maxColumn[i] == terminate) break;
	}
    
	/* Trace the alignment ending position on read. */
	uint8_t *t = (uint8_t*)pvHmax;
	int32_t column_len = segLen * 16;
	for (i = 0; LIKELY(i < column_len); ++i, ++t) {
		int32_t temp;
		if (*t == max) {
			temp = i / 16 + i % 16 * segLen;
			if (temp < end_read) end_read = temp;
		}
	}
    
	/* Find the most possible 2nd best alignment. */
	alignment_end* bests = (alignment_end*) calloc(2, sizeof(alignment_end));
	bests[0].score = max + bias >= 255 ? 255 : max;
	bests[0].ref = end_ref;
	bests[0].read = end_read;
    
	bests[1].score = 0;
	bests[1].ref = 0;
	bests[1].read = 0;
    
	edge = (end_ref - maskLen) > 0 ? (end_ref - maskLen) : 0;
	for (i = 0; i < edge; i ++) {
        //			fprintf (stderr, "maxColumn[%d]: %d\n", i, maxColumn[i]);
		if (maxColumn[i] > bests[1].score) {
			bests[1].score = maxColumn[i];
			bests[1].ref = i;
		}
	}
	edge = (end_ref + maskLen) > db_length ? db_length : (end_ref + maskLen);
	for (i = edge + 1; i < db_length; i ++) {
        //			fprintf (stderr, "db_length: %d\tmaxColumn[%d]: %d\n", db_length, i, maxColumn[i]);
		if (maxColumn[i] > bests[1].score) {
			bests[1].score = maxColumn[i];
			bests[1].ref = i;
		}
	}

	return bests;
#undef max16
}


SmithWaterman::alignment_end* SmithWaterman::sw_sse2_word (const int* db_sequence,
                                    int8_t ref_dir,	// 0: forward ref; 1: reverse ref
                                    int32_t db_length,
                                    int32_t query_lenght,
                                    const uint8_t gap_open, /* will be used as - */
                                    const uint8_t gap_extend, /* will be used as - */
                                    const __m128i*query_profile_word,
                                    uint16_t terminate,
                                    int32_t maskLen) {
    
#define max8(m, vm) (vm) = _mm_max_epi16((vm), _mm_srli_si128((vm), 8)); \
(vm) = _mm_max_epi16((vm), _mm_srli_si128((vm), 4)); \
(vm) = _mm_max_epi16((vm), _mm_srli_si128((vm), 2)); \
(m) = _mm_extract_epi16((vm), 0)
    
	uint16_t max = 0;		                     /* the max alignment score */
	int32_t end_read = query_lenght - 1;
	int32_t end_ref = 0; /* 1_based best alignment ending point; Initialized as isn't aligned - 0. */
	int32_t segLen = (query_lenght + 7) / 8; /* number of segment */
	/* array to record the alignment read ending position of the largest score of each reference position */
    memset(this->maxColumn, 0, db_length * sizeof(uint16_t));
    uint16_t * maxColumn = (uint16_t *) this->maxColumn;
    
	/* Define 16 byte 0 vector. */
	__m128i vZero = _mm_set1_epi32(0);
    __m128i* pvHStore = vHStore;
    __m128i* pvHLoad = vHLoad;
    __m128i* pvE = vE;
    __m128i* pvHmax = vHmax;
	memset(pvHStore,0,segLen*sizeof(__m128i));
    memset(pvHLoad,0, segLen*sizeof(__m128i));
    memset(pvE,0,     segLen*sizeof(__m128i));
    memset(pvHmax,0,  segLen*sizeof(__m128i));

	int32_t i, j, k;
	/* 16 byte insertion begin vector */
	__m128i vGapO = _mm_set1_epi16(gap_open);
    
	/* 16 byte insertion extension vector */
	__m128i vGapE = _mm_set1_epi16(gap_extend);
    
	__m128i vMaxScore = vZero; /* Trace the highest score of the whole SW matrix. */
	__m128i vMaxMark = vZero; /* Trace the highest score till the previous column. */
	__m128i vTemp;
	int32_t edge, begin = 0, end = db_length, step = 1;
    
	/* outer loop to process the reference sequence */
	if (ref_dir == 1) {
		begin = db_length - 1;
		end = -1;
		step = -1;
	}
	for (i = begin; LIKELY(i != end); i += step) {
		int32_t cmp;
		__m128i e, vF = vZero; /* Initialize F value to 0.
                                Any errors to vH values will be corrected in the Lazy_F loop.
                                */
		__m128i vH = pvHStore[segLen - 1];
		vH = _mm_slli_si128 (vH, 2); /* Shift the 128-bit value in vH left by 2 byte. */
        
		/* Swap the 2 H buffers. */
		__m128i* pv = pvHLoad;
        
		__m128i vMaxColumn = vZero; /* vMaxColumn is used to record the max values of column i. */
        
		const __m128i* vP = query_profile_word + db_sequence[i] * segLen; /* Right part of the query_profile_byte */
		pvHLoad = pvHStore;
		pvHStore = pv;
        
		/* inner loop to process the query sequence */
		for (j = 0; LIKELY(j < segLen); j ++) {
			vH = _mm_adds_epi16(vH, _mm_load_si128(vP + j));
            
			/* Get max from vH, vE and vF. */
			e = _mm_load_si128(pvE + j);
			vH = _mm_max_epi16(vH, e);
			vH = _mm_max_epi16(vH, vF);
			vMaxColumn = _mm_max_epi16(vMaxColumn, vH);
            
			/* Save vH values. */
			_mm_store_si128(pvHStore + j, vH);
            
			/* Update vE value. */
			vH = _mm_subs_epu16(vH, vGapO); /* saturation arithmetic, result >= 0 */
			e = _mm_subs_epu16(e, vGapE);
			e = _mm_max_epi16(e, vH);
			_mm_store_si128(pvE + j, e);
            
			/* Update vF value. */
			vF = _mm_subs_epu16(vF, vGapE);
			vF = _mm_max_epi16(vF, vH);
            
			/* Load the next vH. */
			vH = _mm_load_si128(pvHLoad + j);
		}
        
		/* Lazy_F loop: has been revised to disallow adjecent insertion and then deletion, so don't update E(i, j), learn from SWPS3 */
		for (k = 0; LIKELY(k < 8); ++k) {
			vF = _mm_slli_si128 (vF, 2);
			for (j = 0; LIKELY(j < segLen); ++j) {
				vH = _mm_load_si128(pvHStore + j);
				vH = _mm_max_epi16(vH, vF);
				_mm_store_si128(pvHStore + j, vH);
				vH = _mm_subs_epu16(vH, vGapO);
				vF = _mm_subs_epu16(vF, vGapE);
				if (UNLIKELY(! _mm_movemask_epi8(_mm_cmpgt_epi16(vF, vH)))) goto end;
			}
		}
        
    end:
		vMaxScore = _mm_max_epi16(vMaxScore, vMaxColumn);
		vTemp = _mm_cmpeq_epi16(vMaxMark, vMaxScore);
		cmp = _mm_movemask_epi8(vTemp);
		if (cmp != 0xffff) {
			uint16_t temp;
			vMaxMark = vMaxScore;
			max8(temp, vMaxScore);
			vMaxScore = vMaxMark;
            
			if (LIKELY(temp > max)) {
				max = temp;
				end_ref = i;
				for (j = 0; LIKELY(j < segLen); ++j) pvHmax[j] = pvHStore[j];
			}
		}
        
		/* Record the max score of current column. */
		max8(maxColumn[i], vMaxColumn);
		if (maxColumn[i] == terminate) break;
	}
    
	/* Trace the alignment ending position on read. */
	uint16_t *t = (uint16_t*)pvHmax;
	int32_t column_len = segLen * 8;
	for (i = 0; LIKELY(i < column_len); ++i, ++t) {
		int32_t temp;
		if (*t == max) {
			temp = i / 8 + i % 8 * segLen;
			if (temp < end_read) end_read = temp;
		}
	}
    
	/* Find the most possible 2nd best alignment. */
    SmithWaterman::alignment_end* bests = (alignment_end*) calloc(2, sizeof(alignment_end));
	bests[0].score = max;
	bests[0].ref = end_ref;
	bests[0].read = end_read;
    
	bests[1].score = 0;
	bests[1].ref = 0;
	bests[1].read = 0;
    
	edge = (end_ref - maskLen) > 0 ? (end_ref - maskLen) : 0;
	for (i = 0; i < edge; i ++) {
		if (maxColumn[i] > bests[1].score) {
			bests[1].score = maxColumn[i];
			bests[1].ref = i;
		}
	}
	edge = (end_ref + maskLen) > db_length ? db_length : (end_ref + maskLen);
	for (i = edge; i < db_length; i ++) {
		if (maxColumn[i] > bests[1].score) {
			bests[1].score = maxColumn[i];
			bests[1].ref = i;
		}
	}
    
	return bests;
#undef max8
}

void SmithWaterman::ssw_init (const Sequence* q,
               const int8_t* mat,
               const int32_t alphabetSize,
               const int8_t score_size) {

    profile->bias = 0;
    
    for(int i = 0; i < q->L; i++){
        profile->query_sequence[i] = (int8_t) q->int_sequence[i];
    }
    if (score_size == 0 || score_size == 2) {
        /* Find the bias to use in the substitution matrix */
        int32_t bias = 0, i;
        for (i = 0; i < alphabetSize*alphabetSize; i++) if (mat[i] < bias) bias = mat[i];
        bias = abs(bias);

        profile->bias = bias;
        createQueryProfile<int8_t,16>(profile->profile_byte, profile->query_sequence, mat, q->L, alphabetSize, bias);
    }
    if (score_size == 1 || score_size == 2) {
        createQueryProfile<int16_t,8>(profile->profile_word, profile->query_sequence, mat, q->L, alphabetSize, 0);
    }
    
    seq_reverse( profile->query_rev_sequence, profile->query_sequence, q->L);
    profile->mat = mat;
    profile->query_length = q->L;
    profile->alphabetSize = alphabetSize;
}

SmithWaterman::cigar* SmithWaterman::banded_sw (const int*db_sequence,
                                                const int8_t* query_sequence,
                                                int32_t db_length,
                                                int32_t query_length,
                                                int32_t score,
                                                const uint32_t gap_open,  /* will be used as - */
                                                const uint32_t gap_extend,  /* will be used as - */
                                                int32_t band_width,
                                                const int8_t* mat,	/* pointer to the weight matrix */
                                                int32_t n) {
    /*! @function
     @abstract  Round an integer to the next closest power-2 integer.
     @param  x  integer to be rounded (in place)
     @discussion x will be modified.
     */
#define kroundup32(x) (--(x), (x)|=(x)>>1, (x)|=(x)>>2, (x)|=(x)>>4, (x)|=(x)>>8, (x)|=(x)>>16, ++(x))
    
    /* Convert the coordinate in the scoring matrix into the coordinate in one line of the band. */
#define set_u(u, w, i, j) { int x=(i)-(w); x=x>0?x:0; (u)=(j)-x+1; }
    
    /* Convert the coordinate in the direction matrix into the coordinate in one line of the band. */
#define set_d(u, w, i, j, p) { int x=(i)-(w); x=x>0?x:0; x=(j)-x; (u)=x*3+p; }
    
	uint32_t *c = (uint32_t*)malloc(16 * sizeof(uint32_t)), *c1;
	int32_t i, j, e, f, temp1, temp2, s = 16, s1 = 8, l, max = 0;
	int64_t s2 = 1024;
	char op, prev_op;
	int32_t width, width_d, *h_b, *e_b, *h_c;
	int8_t *direction, *direction_line;
	cigar* result = new cigar();
	h_b = (int32_t*)malloc(s1 * sizeof(int32_t));
	e_b = (int32_t*)malloc(s1 * sizeof(int32_t));
	h_c = (int32_t*)malloc(s1 * sizeof(int32_t));
	direction = (int8_t*)malloc(s2 * sizeof(int8_t));
    
	do {
		width = band_width * 2 + 3, width_d = band_width * 2 + 1;
		while (width >= s1) {
			++s1;
			kroundup32(s1);
			h_b = (int32_t*)realloc(h_b, s1 * sizeof(int32_t));
			e_b = (int32_t*)realloc(e_b, s1 * sizeof(int32_t));
			h_c = (int32_t*)realloc(h_c, s1 * sizeof(int32_t));
		}
		while (width_d * query_length * 3 >= s2) {
			++s2;
			kroundup32(s2);
			if (s2 < 0) {
				fprintf(stderr, "Alignment score and position are not consensus.\n");
				exit(1);
			}
			direction = (int8_t*)realloc(direction, s2 * sizeof(int8_t));
		}
		direction_line = direction;
		for (j = 1; LIKELY(j < width - 1); j ++) h_b[j] = 0;
		for (i = 0; LIKELY(i < query_length); i ++) {
			int32_t beg = 0, end = db_length - 1, u = 0, edge;
			j = i - band_width;	beg = beg > j ? beg : j; // band start
			j = i + band_width; end = end < j ? end : j; // band end
			edge = end + 1 < width - 1 ? end + 1 : width - 1;
			f = h_b[0] = e_b[0] = h_b[edge] = e_b[edge] = h_c[0] = 0;
			direction_line = direction + width_d * i * 3;
            
			for (j = beg; LIKELY(j <= end); j ++) {
				int32_t b, e1, f1, d, de, df, dh;
				set_u(u, band_width, i, j);	set_u(e, band_width, i - 1, j);
				set_u(b, band_width, i, j - 1); set_u(d, band_width, i - 1, j - 1);
				set_d(de, band_width, i, j, 0);
				set_d(df, band_width, i, j, 1);
				set_d(dh, band_width, i, j, 2);
                
				temp1 = i == 0 ? -gap_open : h_b[e] - gap_open;
				temp2 = i == 0 ? -gap_extend : e_b[e] - gap_extend;
				e_b[u] = temp1 > temp2 ? temp1 : temp2;
				direction_line[de] = temp1 > temp2 ? 3 : 2;
                
				temp1 = h_c[b] - gap_open;
				temp2 = f - gap_extend;
				f = temp1 > temp2 ? temp1 : temp2;
				direction_line[df] = temp1 > temp2 ? 5 : 4;
                
				e1 = e_b[u] > 0 ? e_b[u] : 0;
				f1 = f > 0 ? f : 0;
				temp1 = e1 > f1 ? e1 : f1;
				temp2 = h_b[d] + mat[db_sequence[j] * n + query_sequence[i]];
				h_c[u] = temp1 > temp2 ? temp1 : temp2;
                
				if (h_c[u] > max) max = h_c[u];
                
				if (temp1 <= temp2) direction_line[dh] = 1;
				else direction_line[dh] = e1 > f1 ? direction_line[de] : direction_line[df];
			}
			for (j = 1; j <= u; j ++) h_b[j] = h_c[j];
		}
		band_width *= 2;
	} while (LIKELY(max < score));
	band_width /= 2;
    
	// trace back
	i = query_length - 1;
	j = db_length - 1;
	e = 0;	// Count the number of M, D or I.
	l = 0;	// record length of current cigar
	op = prev_op = 'M';
	temp2 = 2;	// h
	while (LIKELY(i > 0)) {
		set_d(temp1, band_width, i, j, temp2);
		switch (direction_line[temp1]) {
			case 1:
				--i;
				--j;
				temp2 = 2;
				direction_line -= width_d * 3;
				op = 'M';
				break;
			case 2:
			 	--i;
				temp2 = 0;	// e
				direction_line -= width_d * 3;
				op = 'I';
				break;
			case 3:
				--i;
				temp2 = 2;
				direction_line -= width_d * 3;
				op = 'I';
				break;
			case 4:
				--j;
				temp2 = 1;
				op = 'D';
				break;
			case 5:
				--j;
				temp2 = 2;
				op = 'D';
				break;
			default:
				fprintf(stderr, "Trace back error: %d.\n", direction_line[temp1 - 1]);
				free(direction);
				free(h_c);
				free(e_b);
				free(h_b);
				free(c);
				delete result;
				return 0;
		}
		if (op == prev_op) ++e;
		else {
			++l;
			while (l >= s) {
				++s;
				kroundup32(s);
				c = (uint32_t*)realloc(c, s * sizeof(uint32_t));
			}
			c[l - 1] = to_cigar_int(e, prev_op);
			prev_op = op;
			e = 1;
		}
	}
	if (op == 'M') {
		++l;
		while (l >= s) {
			++s;
			kroundup32(s);
			c = (uint32_t*)realloc(c, s * sizeof(uint32_t));
		}
		c[l - 1] = to_cigar_int(e + 1, op);
	}else {
		l += 2;
		while (l >= s) {
			++s;
			kroundup32(s);
			c = (uint32_t*)realloc(c, s * sizeof(uint32_t));
		}
		c[l - 2] = to_cigar_int(e, op);
		c[l - 1] = to_cigar_int(1, 'M');
	}
    
	// reverse cigar
	c1 = (uint32_t*)new uint32_t[l * sizeof(uint32_t)];
	s = 0;
	e = l - 1;
	while (LIKELY(s <= e)) {
		c1[s] = c[e];
		c1[e] = c[s];
		++ s;
		-- e;
	}
	result->seq = c1;
	result->length = l;
    
	free(direction);
	free(h_c);
	free(e_b);
	free(h_b);
	free(c);
	return result;
#undef kroundup32
#undef set_u
#undef set_d
}

uint32_t SmithWaterman::to_cigar_int (uint32_t length, char op_letter)
{
	uint32_t res;
	uint8_t op_code;
    
	switch (op_letter) {
		case 'M': /* alignment match (can be a sequence match or mismatch */
		default:
			op_code = 0;
			break;
		case 'I': /* insertion to the reference */
			op_code = 1;
			break;
		case 'D': /* deletion from the reference */
			op_code = 2;
			break;
		case 'N': /* skipped region from the reference */
			op_code = 3;
			break;
		case 'S': /* soft clipping (clipped sequences present in SEQ) */
			op_code = 4;
			break;
		case 'H': /* hard clipping (clipped sequences NOT present in SEQ) */
			op_code = 5;
			break;
		case 'P': /* padding (silent deletion from padded reference) */
			op_code = 6;
			break;
		case '=': /* sequence match */
			op_code = 7;
			break;
		case 'X': /* sequence mismatch */
			op_code = 8;
			break;
	}
    
	res = (length << 4) | op_code;
	return res;
}

void SmithWaterman::printVector(__m128i v){
    for (int i = 0; i < 8; i++)
       printf("%d ", ((short) (sse2_extract_epi16(v, i)) + 32768));
    std::cout << "\n";
}

void SmithWaterman::printVectorUS(__m128i v){
    for (int i = 0; i < 8; i++)
        printf("%d ", (unsigned short) sse2_extract_epi16(v, i));
    std::cout << "\n";
}

unsigned short SmithWaterman::sse2_extract_epi16(__m128i v, int pos) {
    switch(pos){
        case 0: return _mm_extract_epi16(v, 0);
        case 1: return _mm_extract_epi16(v, 1);
        case 2: return _mm_extract_epi16(v, 2);
        case 3: return _mm_extract_epi16(v, 3);
        case 4: return _mm_extract_epi16(v, 4);
        case 5: return _mm_extract_epi16(v, 5);
        case 6: return _mm_extract_epi16(v, 6);
        case 7: return _mm_extract_epi16(v, 7);
    }
    std::cerr << "Fatal error in QueryScore: position in the vector is not in the legal range (pos = " << pos << ")\n";
    exit(1);
    // never executed
    return 0;
}

