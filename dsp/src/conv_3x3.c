/*
 * conv_3x3.c
 *
 *  Created on: 26 Dec 2016
 *      Author: Gopalakrishna Hegde, NTU Singapore
 */
#include "stdlib.h"
#include "string.h"
#include "dsplib.h"
#include "ext_dsplib.h"
#include "imglib.h"
#include "misc_utils.h"
#include "conv_layer.h"
#include "mem_manager.h"
#include "edma_module.h"

extern unsigned int core_id;

extern FIX_MAP *p_line_buff[2][MAX_SUPPORTED_KER_SIZE];

extern uint8_t far private_temp_buff[PRIVATE_TEMP_BUFF_SIZE];

extern uint8_t far private_conv_buff[PRIVATE_TEMP_BUFF_SIZE];

extern uint8_t far line_buff_ping[MAX_SUPPORTED_KER_SIZE * MAX_INPUT_MAP_WIDTH];

extern uint8_t far line_buff_pong[MAX_SUPPORTED_KER_SIZE * MAX_INPUT_MAP_WIDTH];

extern uint8_t far ker_buff_ping[MAX_SUPPORTED_KER_SIZE * MAX_SUPPORTED_KER_SIZE * MAX_SUPPORTED_INPUT_MAPS];
extern uint8_t far ker_buff_pong[MAX_SUPPORTED_KER_SIZE * MAX_SUPPORTED_KER_SIZE * MAX_SUPPORTED_INPUT_MAPS];

extern FIX_KER *p_conv_ker_buff[2];


// Use this API when the weights are stored in DDR OR pad != 0
STATUS_E dsp_fix_conv_3x3(FIX_MAP *p_input,	// pointer to input maps stored in flattened [maps][row][col] format.
	FIX_KER *p_weight,	// pointer to kernels stored in flattened [no_outputs][no_inputs][ker_size][ker_size] format
	FIX_KER *p_bias,	// pointer to bias units. there are 'no_outputs' bias units
	int in_height,		// input feature map height
	int in_width,		// input feature map width
	int no_inputs,		// number of input feature maps
	int no_outputs,		// number of output feature maps
	int start_map,		// map offset to start fot this core.
	int no_maps, 		// no of feature maps assigned to this core.
	int pad,			// padding in all 4 sides. Only equal padding in all dimensions is supported.
	int stride,			// convolution window stride in both horizontal and vertical direction.
	int shift,			// Shifts used for 16b fixed point conversion. Perform shift before adding bias.
	FIX_MAP *p_output	// pointer to output feature maps. Stored in [map][row][col] flattened manner.
	) {
	int o_h, o_w, omap, imap, o_w_x8;
	int pitch, in_row, out_row, r;
	FIX_MAP *p_temp_out_buff;
	Bool use_dma;
	EDMA_OBJ_T * p_edma;
	STATUS_E status = FAILED;

	o_h = (in_height + 2 * pad - 2 + stride - 1) / stride;
	o_w = (in_width  + 2 * pad - 2 + stride - 1) / stride;
	p_temp_out_buff = (FIX_MAP *)private_temp_buff;
	// reset the output maps buffer; only the portion that belongs to this core.
	memset(p_output + start_map * o_h * o_w, 0, o_h * o_w * no_maps * sizeof(FIX_MAP));

	o_w_x8 = in_width + 2 * pad - 2;
	o_w_x8 = (o_w_x8 % 8 == 0)? o_w_x8 : o_w_x8 + (8 - o_w_x8 % 8);

	pitch = in_width + 2 * pad;
	pitch = (pitch % 2 == 0) ? pitch : pitch + 1;

	// We will use EDMA only for off-chip memory transfer. Simple memcpy() is
	// found to be faster than EDMA for small on-chip transfers.
	use_dma = is_dram_addr((Uint32)p_weight);
	p_edma = &shared_edma_obj[core_id * NO_CHANNELS_PER_CORE + 0];	// using second channel allocated for each core for weight transfer

	for(r = 0; r < 3; r++) {
		p_line_buff[0][r]  = (FIX_MAP *)line_buff_ping + r * pitch;
		p_line_buff[1][r]  = (FIX_MAP *)line_buff_pong + r * pitch;
	}
	p_conv_ker_buff[0] = (FIX_KER *)global_address(ker_buff_ping);
	p_conv_ker_buff[1] = (FIX_KER *)global_address(ker_buff_pong);

	// preload the kernels corresponding to first output map
	if(use_dma) {
		dma_array(p_edma, p_weight + start_map * no_inputs * 9, p_conv_ker_buff[start_map % 2], no_inputs * 9 * sizeof(FIX_KER));
		wait_for_dma_tx(p_edma, FALSE, FALSE);
	} else {
		memcpy(p_conv_ker_buff[start_map % 2], p_weight + start_map * no_inputs * 9, no_inputs * 9 * sizeof(FIX_KER));
	}

	for(omap = start_map; omap < start_map + no_maps; omap++) {
		// double buffering of kernel weights
		if(omap < start_map + no_maps - 1) {
			if(use_dma) {
				dma_array(p_edma, p_weight + (omap + 1) * no_inputs * 9, p_conv_ker_buff[(omap + 1) % 2], no_inputs * 9 * sizeof(FIX_KER));
			} else {
				memcpy(p_conv_ker_buff[(omap + 1) % 2], p_weight + (omap + 1) * no_inputs * 9, no_inputs * 9 * sizeof(FIX_KER));
			}
		}
		for(imap = 0; imap < no_inputs; imap++) {
			in_row = -pad;
			memset(line_buff_ping, 0, 3 * pitch * sizeof(FIX_MAP));
			for(out_row = 0; out_row < o_h; out_row++) {
				// we will use memcpy to load the input rows from MSMC to L2. This onchip transfer is
				// empirically found to be faster than EDMA for transfer sizes lesser than 4kB
				if(is_a_ge_zero_and_a_lt_b(in_row, in_height - 2)) {
					// need to load all K rows	starting at line_buff[0]
					for(r = 0; r < 3; r++) {
						memcpy(p_line_buff[0][r] + pad, p_input + (imap * in_height + in_row + r) * in_width, in_width * sizeof(FIX_MAP));
					}
				} else {
					// Need to load only few rows
					if(in_row < 0) { // need to load K + in_row number of rows starting at line buffer no
						for(r = 0; r < 3 + in_row; r++) {
							memcpy(p_line_buff[0][-in_row + r] + pad, p_input + (imap * in_height + r) * in_width, in_width * sizeof(FIX_MAP));
						}
					} else { // bottom end of input map
						// reset the line buffers to mimic zero padding since it is overwritten by the input maps after initial reset.
						memset(line_buff_ping, 0, 3 * pitch * sizeof(FIX_MAP));
						for(r = 0; r < in_height - in_row; r++) {
							memcpy(p_line_buff[0][r] + pad, p_input + (imap * in_height + in_row + r) * in_width, in_width * sizeof(FIX_MAP));
						}
					}
				}
#ifndef USE_IMG_CORR
				IMG_conv_3x3_i16s_c16s(p_line_buff[0][0], // must be 16bit aligned
					p_temp_out_buff,	// must be 32bit aligned
					o_w_x8,				// must be x2
					pitch,				// must be x2
					p_conv_ker_buff[omap % 2] + imap * 9,
					shift
					);
#else
				IMG_corr_3x3_i16s_c16s_short(p_line_buff[0][0], // must be 16bit aligned
					p_temp_out_buff,	// must be 64bit aligned
					o_w_x8,				// must be x4
					pitch,				//
					p_conv_ker_buff[omap % 2] + imap * 9,
					shift
					);
#endif // USE_IMG_CORR
				// throw unnecessary pixels to mimic column stride
				if(stride != 1) {
					strided_move(p_temp_out_buff, o_w, stride);
				}

				memcpy(private_conv_buff, p_output + (omap * o_h + out_row) * o_w, o_w * sizeof(FIX_MAP));
				DSP_add16((FIX_MAP *)private_conv_buff, p_temp_out_buff, (FIX_MAP *)private_conv_buff, o_w_x8);
				memcpy(p_output + (omap * o_h + out_row) * o_w, private_conv_buff, o_w * sizeof(FIX_MAP));

				in_row += stride;
			}
		}
		// add bias
		DSP_vs_add_unroll_8(p_output + omap * o_h * o_w, p_bias[omap], o_w * o_h);

		// wait for the previously initiated weight transfer
		if((omap < start_map + no_maps - 1) && (use_dma == 1)) {
			wait_for_dma_tx(p_edma, FALSE, FALSE);
		}
	}
	return status;
}

// Use this when pad == 0 AND in_width % 2 == 0 AND weights are stored in on-chip memory
STATUS_E dsp_fix_conv_3x3_constrained(FIX_MAP *p_input,	// pointer to input maps stored in flattened [maps][row][col] format.
	FIX_KER *p_weight,	// pointer to kernels stored in flattened [no_outputs][no_inputs][ker_size][ker_size] format
	FIX_KER *p_bias,	// pointer to bias units. there are 'no_outputs' bias units
	int in_height,		// input feature map height
	int in_width,		// input feature map width
	int no_inputs,		// number of input feature maps
	int no_outputs,		// number of output feature maps
	int start_map,		// map offset to start fot this core.
	int no_maps, 		// no of feature maps assigned to this core.
	int stride,			// convolution window stride in both horizontal and vertical direction.
	int shift,			// Shifts used for 16b fixed point conversion. Perform shift before adding bias.
	FIX_MAP *p_output	// pointer to output feature maps. Stored in [map][row][col] flattened manner.
	) {

	int o_h, o_w, omap, imap, o_w_x8, new_width;
	int row;
	STATUS_E status = FAILED;

	o_h = (in_height - 2 + stride - 1) / stride;
	o_w = (in_width  - 2 + stride - 1) / stride;

	new_width = in_width - 2;
#ifndef USE_IMG_CORR
	// The output width for  IMG_conv_3x3_i16s_c16s API must be multiple of 2.
	new_width = ((new_width & 0x1) == 0)? new_width : (new_width + 1);
#else
	// The output width for  IMG_corr_3x3_i16s_c16s_short API must be multiple of 4.
		new_width = ((new_width & 0x3) == 0)? new_width : new_width + (4 - new_width % 4);
#endif
	// reset the output maps buffer; only the portion that belongs to this core.
	memset(p_output + start_map * o_h * o_w, 0, o_h * o_w * no_maps * sizeof(FIX_MAP));
	// DSP_add16 required the array length to mubr x8
	o_w_x8 = in_width - 2;
	o_w_x8 = (o_w_x8 % 8 == 0)? o_w_x8 : o_w_x8 + (8 - o_w_x8 % 8);

	for(omap = start_map; omap < start_map + no_maps; omap++) {
		for(imap = 0; imap < no_inputs; imap++) {
			for(row = 0; row < in_height - 2; row += stride) {
#ifndef USE_IMG_CORR
				IMG_conv_3x3_i16s_c16s(
					p_input + (imap * in_height + row ) * in_width,
					(FIX_MAP *)private_temp_buff,
					new_width,
					in_width,	// must be x2
					p_weight + (omap * no_inputs + imap) * 9,
					shift
					);
#else
				IMG_corr_3x3_i16s_c16s_short(
					p_input + (imap * in_height + row ) * in_width,
					(FIX_MAP *)private_temp_buff,	// 8 byte aligned
					new_width,	// must be x4
					in_width,
					p_weight + (omap * no_inputs + imap) * 9,
					shift
					);
#endif // USE_IMG_CORR
				// throw unnecessary pixels to mimic column stride
				if(stride != 1) {
					strided_move((FIX_MAP *)private_temp_buff, o_w, stride);
				}
				// add the output corresponding to one input map.
				memcpy(private_conv_buff, p_output + (omap * o_h + row / stride) * o_w, o_w * sizeof(FIX_MAP));
				DSP_add16((FIX_MAP *)private_conv_buff, (FIX_MAP *)private_temp_buff, (FIX_MAP *)private_conv_buff, o_w_x8);
				memcpy(p_output + (omap * o_h + row / stride) * o_w, private_conv_buff, o_w * sizeof(FIX_MAP));
			}
		}
		// add bias
		DSP_vs_add_unroll_8(p_output + omap * o_h * o_w, p_bias[omap], o_w * o_h);
	}
	return status;
}
