#include <button.h>

LOG_MODULE_REGISTER(button, LOG_LEVEL_DBG);

static enum button_state current_button_state = RELEASED;

static struct gpio_callback button_cb_data;

static struct gpio_dt_spec* _button_dt_spec;
static button_callback_t _on_state_change_cb;

int debounce_timeout = 30;


static void debounce_button(struct k_work* work){
	int value = gpio_pin_get_dt(_button_dt_spec);
    if (value < 0) {
        LOG_DBG("Cannot read button: %d\n", value);
        return;
    }

    enum button_state new_state = value ? PRESSED : RELEASED;

    if (new_state != current_button_state) {
        current_button_state = new_state;
        LOG_DBG("Reservation button state: %d\n", (int)new_state);

        if (_on_state_change_cb != NULL) {
            _on_state_change_cb(current_button_state);
        }
    }
}

static K_WORK_DELAYABLE_DEFINE(debounce_work, debounce_button);


void button_event_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins){
	// LOG_DBG("Button event at %" PRIu32 "\n", k_cycle_get_32());
	k_work_reschedule(&debounce_work, K_MSEC(debounce_timeout));
}

int register_button(struct gpio_dt_spec* button_dt_spec, button_callback_t on_state_change_cb) {

    _button_dt_spec = button_dt_spec;
    _on_state_change_cb = on_state_change_cb;

    int ret = gpio_pin_interrupt_configure_dt(button_dt_spec, GPIO_INT_EDGE_BOTH);
    gpio_init_callback(&button_cb_data,  button_event_handler, BIT(button_dt_spec->pin));
    
    return ret | gpio_add_callback(button_dt_spec->port, &button_cb_data);
}


