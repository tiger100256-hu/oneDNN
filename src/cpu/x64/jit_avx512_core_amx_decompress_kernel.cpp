/*******************************************************************************
* Copyright 2019-2020 Intel Corporation
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
*******************************************************************************/
#include <float.h>

#include "common/memory_tracking.hpp"
#include "common/dnnl_thread.hpp"
#include "common/utils.hpp"

#include "cpu/x64/jit_avx512_core_amx_decompress_kernel.hpp"

#define GET_OFF(field) offsetof(jit_decompress_call_s, field)

namespace dnnl {
namespace impl {
namespace cpu {
namespace x64 {

using namespace dnnl::impl::memory_tracking::names;
using namespace dnnl::impl::data_type;
using namespace dnnl::impl::utils;
using namespace Xbyak;

size_t jit_avx512_core_amx_decompress_kernel_t::get_wei_h_step() const {
    return (size_t)jcp.typesize_in * jcp.kw * jcp.ic_block_int_np
            * jcp.oc_block;
}

size_t jit_avx512_core_amx_decompress_kernel_t::get_wei_icb_step() const {
    return (size_t)jcp.typesize_in * jcp.kh * jcp.kw * jcp.ic_block_int_np
            * jcp.oc_block;
}

size_t jit_avx512_core_amx_decompress_kernel_t::get_wei_offset(
        int ocb, int kw) const {
    size_t el_offset = (size_t)kw * jcp.ic_block_int_np * jcp.oc_block;
    size_t raw_oc_subblock_step
            = jcp.kh * jcp.kw * jcp.ic_block_int_np * jcp.oc_block;
    size_t oc_subblock_step = jcp.is_relo
            ? rnd_up(raw_oc_subblock_step, jcp.ic_block_int * jcp.oc_block)
            : raw_oc_subblock_step;
    el_offset += (size_t)ocb * jcp.nb_ic_int * oc_subblock_step;
    return jcp.typesize_in * el_offset;
}

void jit_avx512_core_amx_decompress_kernel_t::generate() {
    preamble();
    mov(wei_ptr, ptr[param1 + GET_OFF(filt)]);
    mov(reg_ptr_decomp_dst, ptr[param1 + GET_OFF(scratch_buf)]);
    mov(reg_ptr_decomp_mask, ptr[param1 + GET_OFF(bitmask_ptr)]);
    lea(reg_ptr_decomp_src, ptr[wei_ptr]);

    int wei_buff_size = jcp.nb_oc_blocking * jcp.nb_ic_int * jcp.kh
            * jcp.kw * jcp.ic_block_int_np * jcp.oc_block;

    for(int block = 0; block < wei_buff_size/1024; block++){
        int wei_offset =  block * 1024;
        int bitmask_off = wei_offset / (1 * 8);

        for (int cl = 0; cl < 16; cl = cl + 4) {
            mov(reg_comp_mask_tmp1, ptr[reg_ptr_decomp_mask + cl * 8 + bitmask_off]);
            kmovq(reg_comp_mask1, reg_comp_mask_tmp1);
            mov(reg_comp_mask_tmp2, ptr[reg_ptr_decomp_mask + (cl+1) * 8 + bitmask_off]);
            kmovq(reg_comp_mask2, reg_comp_mask_tmp2);
            mov(reg_comp_mask_tmp3, ptr[reg_ptr_decomp_mask + (cl+2) * 8 + bitmask_off]);
            kmovq(reg_comp_mask3, reg_comp_mask_tmp3);
            mov(reg_comp_mask_tmp4, ptr[reg_ptr_decomp_mask + (cl+3) * 8 + bitmask_off]);
            kmovq(reg_comp_mask4, reg_comp_mask_tmp4);

            vmovdqu8(zmm_comp1, ptr[reg_ptr_decomp_src]);
            popcnt(reg_popcnt, reg_comp_mask_tmp1);
            add(reg_ptr_decomp_src, reg_popcnt);

            vmovdqu8(zmm_comp2, ptr[reg_ptr_decomp_src]);
            popcnt(reg_popcnt, reg_comp_mask_tmp2);
            add(reg_ptr_decomp_src, reg_popcnt);

            vmovdqu8(zmm_comp3, ptr[reg_ptr_decomp_src]);
            popcnt(reg_popcnt, reg_comp_mask_tmp3);
            add(reg_ptr_decomp_src, reg_popcnt);

            vmovdqu8(zmm_comp4, ptr[reg_ptr_decomp_src]);
            popcnt(reg_popcnt, reg_comp_mask_tmp4);
            add(reg_ptr_decomp_src, reg_popcnt);

            
            vpexpandb(zmm_comp1 | reg_comp_mask1 | T_z, zmm_comp1);
            vmovdqu8(ptr[reg_ptr_decomp_dst + wei_offset + cl * 64], zmm_comp1);

            vpexpandb(zmm_comp2 | reg_comp_mask2 | T_z, zmm_comp2);
            vmovdqu8(ptr[reg_ptr_decomp_dst + wei_offset + (cl+1) * 64], zmm_comp2);

            vpexpandb(zmm_comp3 | reg_comp_mask3 | T_z, zmm_comp3);
            vmovdqu8(ptr[reg_ptr_decomp_dst + wei_offset + (cl+2) * 64], zmm_comp3);

            vpexpandb(zmm_comp4 | reg_comp_mask4 | T_z, zmm_comp4);
            vmovdqu8(ptr[reg_ptr_decomp_dst + wei_offset + (cl+3) * 64], zmm_comp4);
        }        
        mov(reg_ptr_decomp_src_align, reg_ptr_decomp_src);
        not_(reg_ptr_decomp_src_align);
        and_(reg_ptr_decomp_src_align, 0x3f); // get 6 LSBs of stack ptr
        add(reg_ptr_decomp_src_align, 0x1);
        and_(reg_ptr_decomp_src_align, 0x3f); // 0x0 if already aligned to cacheline
        add(reg_ptr_decomp_src, reg_ptr_decomp_src_align); 
    }
    postamble();
}

status_t jit_avx512_core_amx_decompress_kernel_t::init_conf() {
    return status::success;
}

int jit_avx512_core_amx_decompress_kernel_t::get_wei_tensor(int i) const {
    const int W_BASE = 6;
    const int W_LAST = 8;
    // assert(0 <= W_BASE && W_BASE < W_LAST && W_LAST <= jcp.max_tiles);
    MAYBE_UNUSED(W_LAST);
    const int tile = W_BASE + i;
    assert(W_BASE <= tile && tile < W_LAST);
    return tile;
}
void jit_avx512_core_amx_decompress_kernel_t::init_scratchpad(
        memory_tracking::registrar_t &scratchpad,
    const jit_conv_conf_t& jcp, const primitive_attr_t& attr) {

}
} // namespace x64
} // namespace cpu
} // namespace impl
} // namespace dnnl
