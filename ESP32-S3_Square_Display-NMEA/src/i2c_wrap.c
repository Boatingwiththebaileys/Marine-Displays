/**
 * i2c_wrap.c - Linker wrap stubs and compatibility shims
 *
 * Provides:
 *  - __wrap_i2c_driver_install / __wrap_i2c_driver_delete
 *      Pass-through wrappers enabled by -Wl,--wrap= flags. Allows the project
 *      to intercept early Arduino/IDF I2C driver lifecycle calls without
 *      changing behaviour.
 *
 */

#include "driver/i2c.h"
#include "esp_err.h"

/* ----------------------------------------------------------------------------
 * I2C driver linker wraps
 * --------------------------------------------------------------------------*/

/* Forward-declare the real IDF functions so the wraps can call them. */
esp_err_t __real_i2c_driver_install(i2c_port_t i2c_num, i2c_mode_t mode,
                                    size_t slv_rx_buf_len, size_t slv_tx_buf_len,
                                    int intr_alloc_flags);
esp_err_t __real_i2c_driver_delete(i2c_port_t i2c_num);

esp_err_t __wrap_i2c_driver_install(i2c_port_t i2c_num, i2c_mode_t mode,
                                    size_t slv_rx_buf_len, size_t slv_tx_buf_len,
                                    int intr_alloc_flags)
{
    return __real_i2c_driver_install(i2c_num, mode, slv_rx_buf_len, slv_tx_buf_len, intr_alloc_flags);
}

esp_err_t __wrap_i2c_driver_delete(i2c_port_t i2c_num)
{
    return __real_i2c_driver_delete(i2c_num);
}

/* check_i2c_driver_conflict - wrapped but never actually called in this build.
 * Provide a safe no-op so the symbol resolves if referenced. */
bool __wrap_check_i2c_driver_conflict(void)
{
    return false;
}
