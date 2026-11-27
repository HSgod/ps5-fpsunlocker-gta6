/* Host self-check for compute_resolution(): cc -DHOST_TEST test_resolution.c && ./a.out */
#define main unlock_main
#include "gtavi-unlockfps.c"
#undef main
#include <assert.h>

static void run(uint32_t pct, uint32_t highres, uint32_t out) {
    g_res_pct = pct; g_pro_highres = highres; g_output_res = out;
    compute_resolution();
    printf("res_pct=%u highres=%u out=%u -> %ux%u scale %.6f heap %u pages\n",
           pct, highres, out, g_res_w, g_res_h, (double)g_scale, g_heap_pages);
}

int main(void) {
    run(67, 1, 0);  /* measured clean: 2574x1448 */
    assert(g_res_w == 2574 && g_res_h == 1448 && g_heap_pages > HEAP_PAGES_STOCK && g_heap_pages <= 0x3900);
    run(70, 1, 0);  /* measured clean with 0x4000 pages: 2688x1512, no array move */
    assert(g_res_w == 2688 && g_res_h == 1512 && g_heap_pages <= HEAP_PAGES_ARRAY);
    run(80, 1, 0);  /* measured clean: 3072x1728 with 0x5c00 pages (array moved) */
    assert(g_res_w == 3072 && g_res_h == 1728 && g_heap_pages > HEAP_PAGES_ARRAY && g_heap_pages <= 0x5c00);
    run(90, 1, 0);  /* measured clean: 3456x1944 with 0x7880 pages */
    assert(g_res_w == 3456 && g_res_h == 1944 && g_heap_pages <= 0x7900);
    run(100, 1, 0); /* measured clean: native 3840x2160 with 0x9a40 pages */
    assert(g_res_w == 3840 && g_res_h == 2160 && g_heap_pages <= 0x9a40 && g_heap_pages <= HEAP_PAGES_MAX);
    run(60, 1, 0);  /* below stock: no heap change */
    assert(g_res_w == 2304 && g_res_h == 1296 && g_heap_pages == 0);
    run(67, 0, 0);  /* without pro_highres: even size, no heap */
    assert(!(g_res_w & 1) && !(g_res_h & 1) && g_heap_pages == 0);
    run(50, 0, 1440);
    assert(!(g_res_w & 1) && !(g_res_h & 1));
    run(0, 1, 0);
    assert(g_scale == 0.0f && g_heap_pages == 0);
    puts("all resolution checks passed");
    return 0;
}
