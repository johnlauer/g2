/*
 * cycle_homing.cpp - homing cycle extension to canonical_machine
 * This file is part of the TinyG project
 *
 * Copyright (c) 2010 - 2015 Alden S. Hart, Jr.
 * Copyright (c) 2013 - 2015 Robert Giseburt
 *
 * This file ("the software") is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 as published by the
 * Free Software Foundation. You should have received a copy of the GNU General Public
 * License, version 2 along with the software.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, you may use this file as part of a software library without
 * restriction. Specifically, if other files instantiate templates or use macros or
 * inline functions from this file, or you compile this file and link it with  other
 * files to produce an executable, this file does not by itself cause the resulting
 * executable to be covered by the GNU General Public License. This exception does not
 * however invalidate any other reasons why the executable file might be covered by the
 * GNU General Public License.
 *
 * THE SOFTWARE IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL, BUT WITHOUT ANY
 * WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "tinyg2.h"
#include "util.h"
#include "config.h"
#include "json_parser.h"
#include "text_parser.h"
#include "canonical_machine.h"
#include "planner.h"
#include "gpio.h"
#include "report.h"

/**** Homing singleton structure ****/

struct hmHomingSingleton {			// persistent homing runtime variables

	// controls for homing cycle
	int8_t axis;					// axis currently being homed
	int8_t homing_input;			// homing input for current axis
    uint8_t set_coordinates;		// G28.4 flag. true = set coords to zero at the end of homing cycle
	stat_t (*func)(int8_t axis);	// binding for callback function state machine

	// per-axis parameters
	float direction;				// set to 1 for positive (max), -1 for negative (to min);
	float search_travel;			// signed distance to travel in search
	float search_velocity;			// search speed as positive number
	float latch_velocity;			// latch speed as positive number
	float latch_backoff;			// max distance to back off switch during latch phase
	float zero_backoff;				// distance to back off switch before setting zero
	float max_clear_backoff;		// maximum distance of switch clearing backoffs before erring out

	// state saved from gcode model
	uint8_t saved_units_mode;		// G20,G21 global setting
	uint8_t saved_coord_system;		// G54 - G59 setting
	uint8_t saved_distance_mode;	// G90, G91 global setting
	uint8_t saved_feed_rate_mode;	// G93, G94 global setting
	float saved_feed_rate;			// F setting
	float saved_jerk;				// saved and restored for each axis homed
//    bool saved_limit_enable;        // limit switch processing / overrride
};
static struct hmHomingSingleton hm;

/**** NOTE: global prototypes and other .h info is located in canonical_machine.h ****/

static stat_t _set_homing_func(stat_t (*func)(int8_t axis));
static stat_t _homing_axis_start(int8_t axis);
static stat_t _homing_axis_clear(int8_t axis);
static stat_t _homing_axis_search(int8_t axis);
static stat_t _homing_axis_latch(int8_t axis);
static stat_t _homing_axis_zero_backoff(int8_t axis);
static stat_t _homing_axis_set_zero(int8_t axis);
static stat_t _homing_axis_move(int8_t axis, float target, float velocity);
static stat_t _homing_error_exit(int8_t axis, stat_t status);
static stat_t _homing_finalize_exit(int8_t axis);
static int8_t _get_next_axis(int8_t axis);

/**** HELPERS ***************************************************************************
 * _set_homing_func() - a convenience for setting the next dispatch vector and exiting
 */

static stat_t _set_homing_func(stat_t (*func)(int8_t axis))
{
    hm.func = func;
    return (STAT_EAGAIN);
}

/***********************************************************************************
 **** G28.2 Homing Cycle ***********************************************************
 ***********************************************************************************/

/*****************************************************************************
 * cm_homing_cycle_start()	    - G28.2 homing cycle using limit switches
 * cm_homing_cycle_callback()   - main loop callback for running the homing cycle
 *
 * Homing works from a G28.2 according to the following writeup:
 *	https://github.com/synthetos/TinyG/wiki/TinyG-Homing-(version-0.95-and-above)
 *
 *	--- How does this work? ---
 *
 *	Homing is invoked using a G28.2 command with one or more axes specified in the
 *	command: e.g. g28.2 x0 y0 z0   (FYI: the number after each axis is irrelevant)
 *
 *	To enable an axis for homing the Homing Input (hi) must be set to a valid input
 *  and that input configured for the proper switch type (NO, NC). It is preferable
 *  to have a unique input for each homing axis but it is possible to share an input
 *  across two or more axes. In this case the homing routine cannot automatically
 *  back off a homing switch that is fired at the start of the homing cycle.
 *
 *	Homing is always run in the following order - for each enabled axis:
 *	  Z,X,Y,A,B,C
 *
 *	After initialization the following sequence is run for each axis to be homed:
 *
 *    0. Limits are automatically disabled. Shutdown and safety interlocks are not.
 *	  1. If a homing input is active on invocation, clear off the input (switch)
 *	  2. Search towards homing switch in the set direction until switch is hit
 *	  2. Move off the homing switch at latch velocity until switch opens
 *	  3. Back off switch by the zero backoff distance and set zero for that axis
 *
 *	Homing works as a state machine that is driven by registering a callback function
 *  at hm.func() for the next state to be run. Once the axis is initialized each
 *  callback basically does two things (1) start the move for the current function,
 *  and (2) register the next state with hm.func(). When a move is started it will
 *  either be interrupted if the homing switch changes state. This will cause the
 *  move to stop with a feedhold. The other thing that can happen is the move will
 *  run to its full length if no switch change is detected (hit or open).
 *
 *	Once all moves for an axis are complete the next axis in the sequence is homed
 *
 *	When a homing cycle is initiated the homing state is set to HOMING_NOT_HOMED
 *	When homing completes successfully this is set to HOMING_HOMED, otherwise it
 *	remains HOMING_NOT_HOMED.
 */
/*	--- Some further details ---
 *
 *	Note: When coding a cycle (like this one) you get to perform one queued
 *	move per entry into the continuation, then you must exit.
 *
 *	Another Note: When coding a cycle (like this one) you must wait until
 *	the last move has actually been queued (or has finished) before declaring
 *	the cycle to be done. Otherwise there is a nasty race condition in
 *	_controller_HSM() that may accept the next command before the position of
 *	the final move has been recorded in the Gcode model. That's what the call
 *	to cm_get_runtime_busy() is about.
 */

stat_t cm_homing_cycle_start(void)
{
	// save relevant non-axis parameters from Gcode model
	hm.saved_units_mode = cm_get_units_mode(ACTIVE_MODEL);
	hm.saved_coord_system = cm_get_coord_system(ACTIVE_MODEL);
	hm.saved_distance_mode = cm_get_distance_mode(ACTIVE_MODEL);
	hm.saved_feed_rate_mode = cm_get_feed_rate_mode(ACTIVE_MODEL);
	hm.saved_feed_rate = cm_get_feed_rate(ACTIVE_MODEL);

	// set working values
	cm_set_units_mode(MILLIMETERS);
	cm_set_distance_mode(INCREMENTAL_MODE);
	cm_set_coord_system(ABSOLUTE_COORDS);	// homing is done in machine coordinates
	cm_set_feed_rate_mode(UNITS_PER_MINUTE_MODE);
	hm.set_coordinates = true;

	hm.axis = -1;							// set to retrieve initial axis
	hm.func = _homing_axis_start; 			// bind initial processing function
	cm.machine_state = MACHINE_CYCLE;
	cm.cycle_state = CYCLE_HOMING;
	cm.homing_state = HOMING_NOT_HOMED;
//    cm.limit_enable = false;                // disable limit switch processing
	return (STAT_OK);
}

stat_t cm_homing_cycle_start_no_set(void)
{
	cm_homing_cycle_start();
	hm.set_coordinates = false;				// set flag to not update position variables at the end of the cycle
	return (STAT_OK);
}

stat_t cm_homing_cycle_callback(void)
{
    if (cm.cycle_state != CYCLE_HOMING) return (STAT_NOOP);     // exit if not in a homing cycle
    if (cm_get_runtime_busy()) return (STAT_EAGAIN);            // sync to planner move ends
    return (hm.func(hm.axis));                                  // execute the current homing move
}

/*
 * Homing axis moves - these execute in sequence for each axis
 *
 *	_homing_axis_start()		- get next axis, initialize variables, call the clear
 *	_homing_axis_clear()		- initiate a clear to move off a switch that is thrown at the start
 *	_homing_axis_search()		- fast search for switch, closes switch
 *	_homing_axis_latch()		- slow reverse until switch opens again
 *	_homing_axis_final()		- backoff from latch location to zero position
 *	_homing_axis_move()			- helper that actually executes the above moves
 */

static stat_t _homing_axis_start(int8_t axis)
{
	// get the first or next axis
	if ((axis = _get_next_axis(axis)) < 0) { 				// axes are done or error
		if (axis == -1) {									// -1 is done
			cm.homing_state = HOMING_HOMED;
			return (_set_homing_func(_homing_finalize_exit));
		} else if (axis == -2) { 							// -2 is error
			return (_homing_error_exit(-2, STAT_HOMING_ERROR_BAD_OR_NO_AXIS));
		}
	}
	// clear the homed flag for axis so we'll be able to move w/o triggering soft limits
	cm.homed[axis] = false;

	// trap axis mis-configurations
	if (fp_ZERO(cm.a[axis].homing_input)) return (_homing_error_exit(axis, STAT_HOMING_ERROR_HOMING_INPUT_MISCONFIGURED));
	if (fp_ZERO(cm.a[axis].search_velocity)) return (_homing_error_exit(axis, STAT_HOMING_ERROR_ZERO_SEARCH_VELOCITY));
	if (fp_ZERO(cm.a[axis].latch_velocity)) return (_homing_error_exit(axis, STAT_HOMING_ERROR_ZERO_LATCH_VELOCITY));
	if (cm.a[axis].latch_backoff < 0) return (_homing_error_exit(axis, STAT_HOMING_ERROR_NEGATIVE_LATCH_BACKOFF));

	// calculate and test travel distance
	float travel_distance = fabs(cm.a[axis].travel_max - cm.a[axis].travel_min) + cm.a[axis].latch_backoff;
	if (fp_ZERO(travel_distance)) return (_homing_error_exit(axis, STAT_HOMING_ERROR_TRAVEL_MIN_MAX_IDENTICAL));

    // Nothing to do about direction now that direction is explicit
    // However, here's a good place to stash the homing_switch:
    hm.homing_input = cm.a[axis].homing_input;
    gpio_set_homing_mode(hm.homing_input, true);
	hm.axis = axis;											// persist the axis
	hm.search_velocity = fabs(cm.a[axis].search_velocity);	// search velocity is always positive
	hm.latch_velocity = fabs(cm.a[axis].latch_velocity);	// latch velocity is always positive

    bool homing_to_max = cm.a[axis].homing_dir;

    // setup parameters for positive or negative travel (homing to the max or min switch)
    if (homing_to_max) {
		hm.search_travel = travel_distance;					// search travels in positive direction
		hm.latch_backoff = -cm.a[axis].latch_backoff;		// latch travels in negative direction
		hm.zero_backoff = -cm.a[axis].zero_backoff;         // ...as does zero backoff
	} else {
		hm.search_travel = -travel_distance;				// search travels in negative direction
		hm.latch_backoff = cm.a[axis].latch_backoff;		// latch travels in positive direction
		hm.zero_backoff = cm.a[axis].zero_backoff;          // ...as does zero backoff
	}

	// if homing is disabled for the axis then skip to the next axis
	hm.saved_jerk = cm_get_axis_jerk(axis);					// save the max jerk value
	return (_set_homing_func(_homing_axis_clear));			// start the clear
}

// Handle an initial switch closure by backing off the closed switch
// NOTE: Relies on independent switches per axis (not shared)
static stat_t _homing_axis_clear(int8_t axis)				// first clear move
{
	if (gpio_read_input(hm.homing_input) == INPUT_ACTIVE) {    // the switch is closed at startup

        // determine if the input switch for this axis is shared w/other axes
        for (uint8_t check_axis = AXIS_X; check_axis < AXES; check_axis++) {
            if (axis != check_axis && cm.a[check_axis].homing_input == hm.homing_input) {
                return (_homing_error_exit(axis, STAT_HOMING_ERROR_MUST_CLEAR_SWITCHES_BEFORE_HOMING)); // axis cannot be homed
            }
        }
        _homing_axis_move(axis, hm.latch_backoff, hm.search_velocity);  // otherwise back off the switch
    }
 	return (_set_homing_func(_homing_axis_search));			// start the search
}

static stat_t _homing_axis_search(int8_t axis)				// start the search
{
	cm_set_axis_jerk(axis, cm.a[axis].jerk_high);			// use the high-speed jerk for search onward
	_homing_axis_move(axis, hm.search_travel, hm.search_velocity);
	return (_set_homing_func(_homing_axis_latch));
}

static stat_t _homing_axis_latch(int8_t axis)				// latch to switch open
{
	mp_flush_planner();                                     // clear out the remaining search move
	_homing_axis_move(axis, hm.latch_backoff, hm.latch_velocity);
	return (_set_homing_func(_homing_axis_zero_backoff));
}

static stat_t _homing_axis_zero_backoff(int8_t axis)		// backoff to zero position
{
    mp_flush_planner();                                     // clear out the remaining latch move
	_homing_axis_move(axis, hm.zero_backoff, hm.search_velocity);
	return (_set_homing_func(_homing_axis_set_zero));
}

static stat_t _homing_axis_set_zero(int8_t axis)			// set zero and finish up
{
	if (hm.set_coordinates) {
		cm_set_position(axis, 0);
		cm.homed[axis] = true;
	} else {                                                // do not set axis if in G28.4 cycle
		cm_set_position(axis, cm_get_work_position(RUNTIME, axis));
	}
	cm_set_axis_jerk(axis, hm.saved_jerk);					// restore the max jerk value

    gpio_set_homing_mode(hm.homing_input, false);           // end homing mode
	return (_set_homing_func(_homing_axis_start));
}

static stat_t _homing_axis_move(int8_t axis, float target, float velocity)
{
	float vect[] = {0,0,0,0,0,0};
	float flags[] = {false, false, false, false, false, false};

	vect[axis] = target;
	flags[axis] = true;
	cm_set_feed_rate(velocity);
	mp_flush_planner();										// don't use cm_request_queue_flush() here
//	if (cm.hold_state == FEEDHOLD_HOLD) {
    cm_end_hold();                                          // ends hold if on is in effect
//    }
	ritorno(cm_straight_feed(vect, flags));
	return (STAT_EAGAIN);
}

/*
 * _homing_error_exit()
 *
 * Generate an error message. Since the error exit returns via the homing callback
 * - and not the main command parser - it requires its own display processing
 */

static stat_t _homing_error_exit(int8_t axis, stat_t status)
{
	nv_reset_nv_list();
	if (axis == -2) {
		nv_add_conditional_message((const char *)"Homing error - Bad or no axis(es) specified");
	} else {
		char message[NV_MESSAGE_LEN];
		sprintf_P(message, PSTR("%c axis %s"), cm_get_axis_char(axis), get_status_message(status));
		nv_add_conditional_message(message);
	}
	nv_print_list(STAT_HOMING_CYCLE_FAILED, TEXT_INLINE_VALUES, JSON_RESPONSE_FORMAT);

	_homing_finalize_exit(axis);
	return (STAT_HOMING_CYCLE_FAILED);						// homing state remains HOMING_NOT_HOMED
}

/*
 * _homing_finalize_exit() - helper to finalize homing
 */

static stat_t _homing_finalize_exit(int8_t axis)			// third part of return to home
{
	mp_flush_planner(); 									// should be stopped, but in case of switch feedhold.
//	if (cm.hold_state == FEEDHOLD_HOLD); {
    cm_end_hold();                                          // ends hold if on is in effect
//    }
	cm_set_coord_system(hm.saved_coord_system);				// restore to work coordinate system
	cm_set_units_mode(hm.saved_units_mode);
	cm_set_distance_mode(hm.saved_distance_mode);
	cm_set_feed_rate_mode(hm.saved_feed_rate_mode);
	cm_set_feed_rate(hm.saved_feed_rate);
	cm_set_motion_mode(MODEL, MOTION_MODE_CANCEL_MOTION_MODE);
	cm_canned_cycle_end();
	return (STAT_OK);
}

/*
 * _get_next_axis() - return next axis in sequence based on axis in arg
 *
 *	Accepts "axis" arg as the current axis; or -1 to retrieve the first axis
 *	Returns next axis based on "axis" argument and if that axis is flagged for homing in the gf struct
 *	Returns -1 when all axes have been processed
 *	Returns -2 if no axes are specified (Gcode calling error)
 *	Homes Z first, then the rest in sequence
 *
 *	Isolating this function facilitates implementing more complex and
 *	user-specified axis homing orders
 */

//#define __ALT_AXES

static int8_t _get_next_axis(int8_t axis)
{
#ifdef __ALT_AXES
// alternate code:
	uint8_t axis;
	for(axis = AXIS_X; axis < HOMING_AXES; axis++)
		if(fp_TRUE(cm.gf.target[axis])) break;
	if(axis >= HOMING_AXES) return -2;
	  switch(axis) {
		case -1:        if (fp_TRUE(cm.gf.target[AXIS_Z])) return (AXIS_Z);
		case AXIS_Z:    if (fp_TRUE(cm.gf.target[AXIS_X])) return (AXIS_X);
		case AXIS_X:    if (fp_TRUE(cm.gf.target[AXIS_Y])) return (AXIS_Y);
		case AXIS_Y:    if (fp_TRUE(cm.gf.target[AXIS_A])) return (AXIS_A);
#if (HOMING_AXES > 4)
		case AXIS_A:    if (fp_TRUE(cm.gf.target[AXIS_B])) return (AXIS_B);
		case AXIS_B:    if (fp_True(cm.gf.target[AXIS_C])) return (AXIS_C);
#endif
		default:        return -1;
	}
#else // __ALT_AXES
#if (HOMING_AXES <= 4)
	if (axis == -1) {	// inelegant brute force solution
		if (fp_TRUE(cm.gf.target[AXIS_Z])) return (AXIS_Z);
		if (fp_TRUE(cm.gf.target[AXIS_X])) return (AXIS_X);
		if (fp_TRUE(cm.gf.target[AXIS_Y])) return (AXIS_Y);
		if (fp_TRUE(cm.gf.target[AXIS_A])) return (AXIS_A);
		return (-2);	// error
	} else if (axis == AXIS_Z) {
		if (fp_TRUE(cm.gf.target[AXIS_X])) return (AXIS_X);
		if (fp_TRUE(cm.gf.target[AXIS_Y])) return (AXIS_Y);
		if (fp_TRUE(cm.gf.target[AXIS_A])) return (AXIS_A);
	} else if (axis == AXIS_X) {
		if (fp_TRUE(cm.gf.target[AXIS_Y])) return (AXIS_Y);
		if (fp_TRUE(cm.gf.target[AXIS_A])) return (AXIS_A);
	} else if (axis == AXIS_Y) {
		if (fp_TRUE(cm.gf.target[AXIS_A])) return (AXIS_A);
	}
	return (-1);	// done

#else
	if (axis == -1) {
		if (fp_TRUE(cm.gf.target[AXIS_Z])) return (AXIS_Z);
		if (fp_TRUE(cm.gf.target[AXIS_X])) return (AXIS_X);
		if (fp_TRUE(cm.gf.target[AXIS_Y])) return (AXIS_Y);
		if (fp_TRUE(cm.gf.target[AXIS_A])) return (AXIS_A);
		if (fp_TRUE(cm.gf.target[AXIS_B])) return (AXIS_B);
		if (fp_TRUE(cm.gf.target[AXIS_C])) return (AXIS_C);
		return (-2);	// error
	} else if (axis == AXIS_Z) {
		if (fp_TRUE(cm.gf.target[AXIS_X])) return (AXIS_X);
		if (fp_TRUE(cm.gf.target[AXIS_Y])) return (AXIS_Y);
		if (fp_TRUE(cm.gf.target[AXIS_A])) return (AXIS_A);
		if (fp_TRUE(cm.gf.target[AXIS_B])) return (AXIS_B);
		if (fp_TRUE(cm.gf.target[AXIS_C])) return (AXIS_C);
	} else if (axis == AXIS_X) {
		if (fp_TRUE(cm.gf.target[AXIS_Y])) return (AXIS_Y);
		if (fp_TRUE(cm.gf.target[AXIS_A])) return (AXIS_A);
		if (fp_TRUE(cm.gf.target[AXIS_B])) return (AXIS_B);
		if (fp_TRUE(cm.gf.target[AXIS_C])) return (AXIS_C);
	} else if (axis == AXIS_Y) {
		if (fp_TRUE(cm.gf.target[AXIS_A])) return (AXIS_A);
		if (fp_TRUE(cm.gf.target[AXIS_B])) return (AXIS_B);
		if (fp_TRUE(cm.gf.target[AXIS_C])) return (AXIS_C);
	} else if (axis == AXIS_A) {
		if (fp_TRUE(cm.gf.target[AXIS_B])) return (AXIS_B);
		if (fp_TRUE(cm.gf.target[AXIS_C])) return (AXIS_C);
	} else if (axis == AXIS_B) {
		if (fp_TRUE(cm.gf.target[AXIS_C])) return (AXIS_C);
	}
	return (-1);	// done

#endif //  (HOMING_AXES <= 4)
#endif // __ALT_AXES
}

/*
 * _get_next_axes() - return next axis in sequence based on axis in arg
 *
 *	Accepts "axis" arg as the current axis; or -1 to retrieve the first axis
 *	Returns next axis based on "axis" argument
 *	Returns -1 when all axes have been processed
 *	Returns -2 if no axes are specified (Gcode calling error)
 *
 *	hm.axis2 is set to the secondary axis if axis is a dual axis
 *	hm.axis2 is set to -1 otherwise
 *
 *	Isolating this function facilitates implementing more complex and
 *	user-specified axis homing orders
 *
 *	Note: the logic to test for disabled or inhibited axes will allow the
 *	following condition to occur: A single axis is specified but it is
 *	disabled or inhibited - homing will say that it was successfully homed.
 */

// _run_homing_dual_axis() - kernal routine for running homing on a dual axis
//static stat_t _run_homing_dual_axis(int8_t axis) { return (STAT_OK);}

/*
int8_t _get_next_axes(int8_t axis)
{
	int8_t next_axis;
	hm.axis2 = -1;

	// Scan target vector for case where no valid axes are specified
	for (next_axis = 0; next_axis < AXES; next_axis++) {
		if ((fp_TRUE(cm.gf.target[next_axis])) &&
			(cm.a[next_axis].axis_mode != AXIS_INHIBITED) &&
			(cm.a[next_axis].axis_mode != AXIS_DISABLED)) {
			break;
		}
	}
	if (next_axis == AXES) {
//		fprintf_P(stderr, PSTR("***** Homing failed: none or disabled/inhibited axes specified\n"));
		return (-2);	// didn't find any axes to process
	}

	// Scan target vector from the current axis to find next axis or the end
	for (next_axis = ++axis; next_axis < AXES; next_axis++) {
		if (fp_TRUE(cm.gf.target[next_axis])) {
			if ((cm.a[next_axis].axis_mode == AXIS_INHIBITED) ||
				(cm.a[next_axis].axis_mode == AXIS_DISABLED)) {	// Skip if axis disabled or inhibited
				continue;
			}
			break;		// got a good one
		}
		return (-1);	// you are done
	}

	// Got a valid axis. Find out if it's a dual
	return (STAT_OK);
}
*/
