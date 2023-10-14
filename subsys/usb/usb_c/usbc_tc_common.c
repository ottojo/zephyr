/*
 * Copyright (c) 2022 The Chromium OS Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(usbc_stack, CONFIG_USBC_STACK_LOG_LEVEL);

#include "usbc_stack.h"
#include "usbc_tc_snk_states_internal.h"
#include "usbc_tc_src_states_internal.h"
#include "usbc_tc_common_internal.h"

static const struct smf_state tc_states[TC_STATE_COUNT];
static void tc_init(const struct device *dev);

/**
 * @brief Initializes the state machine and enters the Disabled state
 */
void tc_subsys_init(const struct device *dev)
{
	struct usbc_port_data *data = dev->data;
	struct tc_sm_t *tc = data->tc;

	/* Save the port device object so states can access it */
	tc->dev = dev;

	/* Initialize the state machine */
    LOG_INF("Initializing Type-C state machine in TC_DISABLED_STATE");
	smf_set_initial(SMF_CTX(tc), &tc_states[TC_DISABLED_STATE]);
}

/**
 * @brief Runs the Type-C layer
 */
void tc_run(const struct device *dev, const int32_t dpm_request)
{
	struct usbc_port_data *data = dev->data;
	const struct device *tcpc = data->tcpc;
	struct tc_sm_t *tc = data->tc;

	/* These requests are implicitly set by the Device Policy Manager */
	if (dpm_request == PRIV_PORT_REQUEST_START) {
		data->tc_enabled = true;
	} else if (dpm_request == PRIV_PORT_REQUEST_SUSPEND) {
		data->tc_enabled = false;
		tc_set_state(dev, TC_DISABLED_STATE);
	}

	switch (data->tc_sm_state) {
	case SM_PAUSED:
		if (data->tc_enabled == false) {
			break;
		}
	/* fall through */
	case SM_INIT:
		/* Initialize the Type-C layer */
		tc_init(dev);
		data->tc_sm_state = SM_RUN;
	/* fall through */
	case SM_RUN:
		if (data->tc_enabled == false) {
			tc_pd_enable(dev, false);
			data->tc_sm_state = SM_PAUSED;
			break;
		}

		/* Sample CC lines */
		tcpc_get_cc(tcpc, &tc->cc1, &tc->cc2);

		/* Detect polarity */
		tc->cc_polarity = (tc->cc1 > tc->cc2) ? TC_POLARITY_CC1 : TC_POLARITY_CC2;

		/* Execute any asyncronous Device Policy Manager Requests */
		if (dpm_request == REQUEST_TC_ERROR_RECOVERY) {
			/* Transition to Error Recovery State */
			tc_set_state(dev, TC_ERROR_RECOVERY_STATE);
		} else if (dpm_request == REQUEST_TC_DISABLED) {
			/* Transition to Disabled State */
			tc_set_state(dev, TC_DISABLED_STATE);
		}

		/* Run state machine */
		smf_run_state(SMF_CTX(tc));
	}
}

/**
 * @brief Checks if the TC Layer is in an Attached state
 */
bool tc_is_in_attached_state(const struct device *dev)
{
#ifdef CONFIG_USBC_CSM_SINK_ONLY
	return (tc_get_state(dev) == TC_ATTACHED_SNK_STATE);
#else
	return (tc_get_state(dev) == TC_ATTACHED_SRC_STATE);
#endif
}

/**
 * @brief Initializes the Type-C layer
 */
static void tc_init(const struct device *dev)
{
	struct usbc_port_data *data = dev->data;
	struct tc_sm_t *tc = data->tc;
	const struct device *tcpc = data->tcpc;

	/* Initialize the timers */
	usbc_timer_init(&tc->tc_t_error_recovery, TC_T_ERROR_RECOVERY_SOURCE_MIN_MS);
    //tc->skip_initial_error_recovery_timer = true;
    //atomic_set_bit(&tc->tc_t_error_recovery.flags, 0);
    //atomic_set_bit(&tc->tc_t_error_recovery.flags, 1);
	usbc_timer_init(&tc->tc_t_cc_debounce, TC_T_CC_DEBOUNCE_MAX_MS);
	usbc_timer_init(&tc->tc_t_rp_value_change, TC_T_RP_VALUE_CHANGE_MAX_MS);
#ifdef CONFIG_USBC_CSM_SOURCE_ONLY
	usbc_timer_init(&tc->tc_t_vconn_off, TC_T_VCONN_OFF_MAX_MS);
#endif

	/* Clear the flags */
	tc->flags = ATOMIC_INIT(0);

	/* Initialize the TCPC */
	tcpc_init(tcpc);

#ifdef CONFIG_USBC_CSM_SOURCE_ONLY
	/* Stop sourcing VBUS */
	data->policy_cb_src_en(dev, false);

	/* Stop sourcing VCONN */
	tcpc_set_vconn(tcpc, false);
#endif

	/* Initialize the state machine */
	/*
	 * Start out in error recovery state so the CC lines are opened for a
	 * short while if this is a system reset.
	 */
    LOG_INF("Initializing Type-C state maching in error recovery state such that CC lines are opened");
	tc_set_state(dev, TC_ERROR_RECOVERY_STATE);
}

/**
 * @brief Sets a Type-C state
 */
void tc_set_state(const struct device *dev, const enum tc_state_t state)
{
	struct usbc_port_data *data = dev->data;
	struct tc_sm_t *tc = data->tc;

	__ASSERT(state < ARRAY_SIZE(tc_states), "invalid tc_state %d", state);
	smf_set_state(SMF_CTX(tc), &tc_states[state]);
}

/**
 * @brief Get the Type-C current state
 */
enum tc_state_t tc_get_state(const struct device *dev)
{
	struct usbc_port_data *data = dev->data;

	return data->tc->ctx.current - &tc_states[0];
}

/**
 * @brief Enable Power Delivery
 */
void tc_pd_enable(const struct device *dev, const bool enable)
{
	if (enable) {
		prl_start(dev);
		pe_start(dev);
	} else {
		prl_suspend(dev);
		pe_suspend(dev);
	}
}

/**
 * @brief TCPC CC/Rp management
 */
void tc_select_src_collision_rp(const struct device *dev, enum tc_rp_value rp)
{
	struct usbc_port_data *data = dev->data;
	const struct device *tcpc = data->tcpc;

	/* Select Rp value */
	tcpc_select_rp_value(tcpc, rp);

	/* Place Rp on CC lines */
	tcpc_set_cc(tcpc, TC_CC_RP);
}

/**
 * @brief CC Open Entry
 */
static void tc_cc_open_entry(void *obj)
{
	struct tc_sm_t *tc = (struct tc_sm_t *)obj;
	const struct device *dev = tc->dev;
	struct usbc_port_data *data = dev->data;
	const struct device *tcpc = data->tcpc;

	tc->cc_voltage = TC_CC_VOLT_OPEN;

	/* Disable VCONN */
	tcpc_set_vconn(tcpc, false);

    LOG_INF("TC_CC_OPEN_ENTRY");
	/* Open CC lines */
	tcpc_set_cc(tcpc, TC_CC_OPEN);
}

/**
 * @brief Disabled Entry
 */
static void tc_disabled_entry(void *obj)
{
	LOG_INF("Entering TC_DISABLED_STATE");
}

/**
 * @brief Disabled Run
 */
static void tc_disabled_run(void *obj)
{
	/* Do nothing */
}

/**
 * @brief ErrorRecovery Entry
 */
static void tc_error_recovery_entry(void *obj)
{
	struct tc_sm_t *tc = (struct tc_sm_t *)obj;

	LOG_INF("ErrorRecovery");

	/* Start tErrorRecovery timer */
    //if(!tc->skip_initial_error_recovery_timer) {
    //    tc->skip_initial_error_recovery_timer = false;
    usbc_timer_start(&tc->tc_t_error_recovery);
    //}
}

/**
 * @brief ErrorRecovery Run
 */
static void tc_error_recovery_run(void *obj)
{
	struct tc_sm_t *tc = (struct tc_sm_t *)obj;
	const struct device *dev = tc->dev;

	/* Wait for expiry */
	if (usbc_timer_expired(&tc->tc_t_error_recovery) == false) {
		return;
	}
    LOG_INF("Error recovery timer expired");

#ifdef CONFIG_USBC_CSM_SINK_ONLY
	/* Transition to Unattached.SNK */
	tc_set_state(dev, TC_UNATTACHED_SNK_STATE);
#else
	/* Transition to Unattached.SRC */
	tc_set_state(dev, TC_UNATTACHED_SRC_STATE);
#endif
}

/**
 * @brief Type-C State Table
 */
static const struct smf_state tc_states[TC_STATE_COUNT] = {
	/* Super States */
	[TC_CC_OPEN_SUPER_STATE] = SMF_CREATE_STATE(
		tc_cc_open_entry,
		NULL,
		NULL,
		NULL),
#ifdef CONFIG_USBC_CSM_SINK_ONLY
	[TC_CC_RD_SUPER_STATE] = SMF_CREATE_STATE(
		tc_cc_rd_entry,
		NULL,
		NULL,
		NULL),
#else
	[TC_CC_RP_SUPER_STATE] = SMF_CREATE_STATE(
		tc_cc_rp_entry,
		NULL,
		NULL,
		NULL),
#endif
	/* Normal States */
#ifdef CONFIG_USBC_CSM_SINK_ONLY
	[TC_UNATTACHED_SNK_STATE] = SMF_CREATE_STATE(
		tc_unattached_snk_entry,
		tc_unattached_snk_run,
		NULL,
		&tc_states[TC_CC_RD_SUPER_STATE]),
	[TC_ATTACH_WAIT_SNK_STATE] = SMF_CREATE_STATE(
		tc_attach_wait_snk_entry,
		tc_attach_wait_snk_run,
		tc_attach_wait_snk_exit,
		&tc_states[TC_CC_RD_SUPER_STATE]),
	[TC_ATTACHED_SNK_STATE] = SMF_CREATE_STATE(
		tc_attached_snk_entry,
		tc_attached_snk_run,
		tc_attached_snk_exit,
		NULL),
#else
	[TC_UNATTACHED_SRC_STATE] = SMF_CREATE_STATE(
		tc_unattached_src_entry,
		tc_unattached_src_run,
		NULL,
		&tc_states[TC_CC_RP_SUPER_STATE]),
	[TC_UNATTACHED_WAIT_SRC_STATE] = SMF_CREATE_STATE(
		tc_unattached_wait_src_entry,
		tc_unattached_wait_src_run,
		tc_unattached_wait_src_exit,
		NULL),
	[TC_ATTACH_WAIT_SRC_STATE] = SMF_CREATE_STATE(
		tc_attach_wait_src_entry,
		tc_attach_wait_src_run,
		tc_attach_wait_src_exit,
		&tc_states[TC_CC_RP_SUPER_STATE]),
	[TC_ATTACHED_SRC_STATE] = SMF_CREATE_STATE(
		tc_attached_src_entry,
		tc_attached_src_run,
		tc_attached_src_exit,
		NULL),
#endif
	[TC_DISABLED_STATE] = SMF_CREATE_STATE(
		tc_disabled_entry,
		tc_disabled_run,
		NULL,
		&tc_states[TC_CC_OPEN_SUPER_STATE]),
	[TC_ERROR_RECOVERY_STATE] = SMF_CREATE_STATE(
		tc_error_recovery_entry,
		tc_error_recovery_run,
		NULL,
		&tc_states[TC_CC_OPEN_SUPER_STATE]),
};
BUILD_ASSERT(ARRAY_SIZE(tc_states) == TC_STATE_COUNT);
