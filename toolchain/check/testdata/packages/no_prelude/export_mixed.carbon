// Part of the Carbon Language project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// AUTOUPDATE

// ============================================================================
// Setup files
// ============================================================================

// --- base.carbon

library "base" api;

class C {
  var x: ();
};

class D {
  var y: ();
};

// --- export_import.carbon

library "export_import" api;

export import library "base";

// --- export_import_then_name.carbon

library "export_import_then_name" api;

import library "export_import";

export C;

// --- export_name.carbon

library "export_name" api;

import library "base";

export C;

// --- export_name_then_import.carbon

library "export_name_then_import" api;

export import library "export_name";

// ============================================================================
// Test files
// ============================================================================

// --- use_export_import_then_name.carbon

library "use_export_import_then_name" api;

import library "export_import_then_name";

var c: C = {.x = ()};

// --- use_export_name_then_import.carbon

library "use_export_name_then_import" api;

import library "export_name_then_import";

var c: C = {.x = ()};

// --- use_both.carbon

library "use_both" api;

import library "export_import_then_name";
import library "export_name_then_import";

var c: C = {.x = ()};

// --- fail_nonexport_use_both.carbon

library "fail_nonexport_use_both" api;

import library "export_import_then_name";
import library "export_name_then_import";

// CHECK:STDERR: fail_nonexport_use_both.carbon:[[@LINE+3]]:8: ERROR: Name `D` not found.
// CHECK:STDERR: var d: D = {.y = ()};
// CHECK:STDERR:        ^
var d: D = {.y = ()};

// --- use_both_reversed.carbon

library "use_both_reversed" api;

import library "export_import_then_name";
import library "export_name_then_import";

var c: C = {.x = ()};

// --- use_both_and_export_import.carbon

library "use_both_and_export_import" api;

import library "export_import_then_name";
import library "export_name_then_import";
import library "export_import";

var c: C = {.x = ()};
var d: D = {.y = ()};

// CHECK:STDOUT: --- base.carbon
// CHECK:STDOUT:
// CHECK:STDOUT: constants {
// CHECK:STDOUT:   %C: type = class_type @C [template]
// CHECK:STDOUT:   %.1: type = tuple_type () [template]
// CHECK:STDOUT:   %.2: type = unbound_element_type C, () [template]
// CHECK:STDOUT:   %.3: type = struct_type {.x: ()} [template]
// CHECK:STDOUT:   %D: type = class_type @D [template]
// CHECK:STDOUT:   %.4: type = unbound_element_type D, () [template]
// CHECK:STDOUT:   %.5: type = struct_type {.y: ()} [template]
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: file {
// CHECK:STDOUT:   package: <namespace> = namespace [template] {
// CHECK:STDOUT:     .C = %C.decl
// CHECK:STDOUT:     .D = %D.decl
// CHECK:STDOUT:   }
// CHECK:STDOUT:   %C.decl: type = class_decl @C [template = constants.%C] {}
// CHECK:STDOUT:   %D.decl: type = class_decl @D [template = constants.%D] {}
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: class @C {
// CHECK:STDOUT:   %.loc5_11.1: () = tuple_literal ()
// CHECK:STDOUT:   %.loc5_11.2: type = converted %.loc5_11.1, constants.%.1 [template = constants.%.1]
// CHECK:STDOUT:   %.loc5_8: <unbound element of class C> = field_decl x, element0 [template]
// CHECK:STDOUT:
// CHECK:STDOUT: !members:
// CHECK:STDOUT:   .Self = constants.%C
// CHECK:STDOUT:   .x = %.loc5_8
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: class @D {
// CHECK:STDOUT:   %.loc9_11.1: () = tuple_literal ()
// CHECK:STDOUT:   %.loc9_11.2: type = converted %.loc9_11.1, constants.%.1 [template = constants.%.1]
// CHECK:STDOUT:   %.loc9_8: <unbound element of class D> = field_decl y, element0 [template]
// CHECK:STDOUT:
// CHECK:STDOUT: !members:
// CHECK:STDOUT:   .Self = constants.%D
// CHECK:STDOUT:   .y = %.loc9_8
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: --- export_import.carbon
// CHECK:STDOUT:
// CHECK:STDOUT: file {
// CHECK:STDOUT:   package: <namespace> = namespace [template] {
// CHECK:STDOUT:     .C = %import_ref.1
// CHECK:STDOUT:     .D = %import_ref.2
// CHECK:STDOUT:   }
// CHECK:STDOUT:   %import_ref.1 = import_ref ir1, inst+1, unloaded
// CHECK:STDOUT:   %import_ref.2 = import_ref ir1, inst+11, unloaded
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: --- export_import_then_name.carbon
// CHECK:STDOUT:
// CHECK:STDOUT: constants {
// CHECK:STDOUT:   %C: type = class_type @C [template]
// CHECK:STDOUT:   %.1: type = tuple_type () [template]
// CHECK:STDOUT:   %.2: type = struct_type {.x: ()} [template]
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: file {
// CHECK:STDOUT:   package: <namespace> = namespace [template] {
// CHECK:STDOUT:     .C = %import_ref.1
// CHECK:STDOUT:     .D = %import_ref.2
// CHECK:STDOUT:   }
// CHECK:STDOUT:   %import_ref.1: type = import_ref ir2, inst+1, loaded [template = constants.%C]
// CHECK:STDOUT:   %import_ref.2 = import_ref ir2, inst+11, unloaded
// CHECK:STDOUT:   %import_ref.3 = import_ref ir2, inst+7, unloaded
// CHECK:STDOUT:   %import_ref.4 = import_ref ir2, inst+2, unloaded
// CHECK:STDOUT:   %C: type = bind_export C, %import_ref.1 [template = constants.%C]
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: class @C {
// CHECK:STDOUT: !members:
// CHECK:STDOUT:   .x = file.%import_ref.3
// CHECK:STDOUT:   .Self = file.%import_ref.4
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: --- export_name.carbon
// CHECK:STDOUT:
// CHECK:STDOUT: constants {
// CHECK:STDOUT:   %C: type = class_type @C [template]
// CHECK:STDOUT:   %.1: type = tuple_type () [template]
// CHECK:STDOUT:   %.2: type = struct_type {.x: ()} [template]
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: file {
// CHECK:STDOUT:   package: <namespace> = namespace [template] {
// CHECK:STDOUT:     .C = %import_ref.1
// CHECK:STDOUT:     .D = %import_ref.2
// CHECK:STDOUT:   }
// CHECK:STDOUT:   %import_ref.1: type = import_ref ir1, inst+1, loaded [template = constants.%C]
// CHECK:STDOUT:   %import_ref.2 = import_ref ir1, inst+11, unloaded
// CHECK:STDOUT:   %import_ref.3 = import_ref ir1, inst+7, unloaded
// CHECK:STDOUT:   %import_ref.4 = import_ref ir1, inst+2, unloaded
// CHECK:STDOUT:   %C: type = bind_export C, %import_ref.1 [template = constants.%C]
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: class @C {
// CHECK:STDOUT: !members:
// CHECK:STDOUT:   .x = file.%import_ref.3
// CHECK:STDOUT:   .Self = file.%import_ref.4
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: --- export_name_then_import.carbon
// CHECK:STDOUT:
// CHECK:STDOUT: file {
// CHECK:STDOUT:   package: <namespace> = namespace [template] {
// CHECK:STDOUT:     .C = %import_ref
// CHECK:STDOUT:   }
// CHECK:STDOUT:   %import_ref = import_ref ir1, inst+11, unloaded
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: --- use_export_import_then_name.carbon
// CHECK:STDOUT:
// CHECK:STDOUT: constants {
// CHECK:STDOUT:   %C: type = class_type @C [template]
// CHECK:STDOUT:   %.1: type = tuple_type () [template]
// CHECK:STDOUT:   %.2: type = struct_type {.x: ()} [template]
// CHECK:STDOUT:   %.3: type = ptr_type {.x: ()} [template]
// CHECK:STDOUT:   %tuple: () = tuple_value () [template]
// CHECK:STDOUT:   %struct: C = struct_value (%tuple) [template]
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: file {
// CHECK:STDOUT:   package: <namespace> = namespace [template] {
// CHECK:STDOUT:     .C = %import_ref.1
// CHECK:STDOUT:     .c = %c
// CHECK:STDOUT:   }
// CHECK:STDOUT:   %import_ref.1: type = import_ref ir1, inst+11, loaded [template = constants.%C]
// CHECK:STDOUT:   %import_ref.2 = import_ref ir1, inst+10, unloaded
// CHECK:STDOUT:   %import_ref.3 = import_ref ir1, inst+9, unloaded
// CHECK:STDOUT:   %C.ref: type = name_ref C, %import_ref.1 [template = constants.%C]
// CHECK:STDOUT:   %c.var: ref C = var c
// CHECK:STDOUT:   %c: ref C = bind_name c, %c.var
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: class @C {
// CHECK:STDOUT: !members:
// CHECK:STDOUT:   .Self = file.%import_ref.2
// CHECK:STDOUT:   .x = file.%import_ref.3
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: fn @__global_init() {
// CHECK:STDOUT: !entry:
// CHECK:STDOUT:   %.loc6_19.1: () = tuple_literal ()
// CHECK:STDOUT:   %.loc6_20.1: {.x: ()} = struct_literal (%.loc6_19.1)
// CHECK:STDOUT:   %.loc6_20.2: ref () = class_element_access file.%c.var, element0
// CHECK:STDOUT:   %.loc6_19.2: init () = tuple_init () to %.loc6_20.2 [template = constants.%tuple]
// CHECK:STDOUT:   %.loc6_20.3: init () = converted %.loc6_19.1, %.loc6_19.2 [template = constants.%tuple]
// CHECK:STDOUT:   %.loc6_20.4: init C = class_init (%.loc6_20.3), file.%c.var [template = constants.%struct]
// CHECK:STDOUT:   %.loc6_21: init C = converted %.loc6_20.1, %.loc6_20.4 [template = constants.%struct]
// CHECK:STDOUT:   assign file.%c.var, %.loc6_21
// CHECK:STDOUT:   return
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: --- use_export_name_then_import.carbon
// CHECK:STDOUT:
// CHECK:STDOUT: constants {
// CHECK:STDOUT:   %C: type = class_type @C [template]
// CHECK:STDOUT:   %.1: type = tuple_type () [template]
// CHECK:STDOUT:   %.2: type = struct_type {.x: ()} [template]
// CHECK:STDOUT:   %.3: type = ptr_type {.x: ()} [template]
// CHECK:STDOUT:   %tuple: () = tuple_value () [template]
// CHECK:STDOUT:   %struct: C = struct_value (%tuple) [template]
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: file {
// CHECK:STDOUT:   package: <namespace> = namespace [template] {
// CHECK:STDOUT:     .C = %import_ref.1
// CHECK:STDOUT:     .c = %c
// CHECK:STDOUT:   }
// CHECK:STDOUT:   %import_ref.1: type = import_ref ir2, inst+11, loaded [template = constants.%C]
// CHECK:STDOUT:   %import_ref.2 = import_ref ir2, inst+10, unloaded
// CHECK:STDOUT:   %import_ref.3 = import_ref ir2, inst+9, unloaded
// CHECK:STDOUT:   %C.ref: type = name_ref C, %import_ref.1 [template = constants.%C]
// CHECK:STDOUT:   %c.var: ref C = var c
// CHECK:STDOUT:   %c: ref C = bind_name c, %c.var
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: class @C {
// CHECK:STDOUT: !members:
// CHECK:STDOUT:   .Self = file.%import_ref.2
// CHECK:STDOUT:   .x = file.%import_ref.3
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: fn @__global_init() {
// CHECK:STDOUT: !entry:
// CHECK:STDOUT:   %.loc6_19.1: () = tuple_literal ()
// CHECK:STDOUT:   %.loc6_20.1: {.x: ()} = struct_literal (%.loc6_19.1)
// CHECK:STDOUT:   %.loc6_20.2: ref () = class_element_access file.%c.var, element0
// CHECK:STDOUT:   %.loc6_19.2: init () = tuple_init () to %.loc6_20.2 [template = constants.%tuple]
// CHECK:STDOUT:   %.loc6_20.3: init () = converted %.loc6_19.1, %.loc6_19.2 [template = constants.%tuple]
// CHECK:STDOUT:   %.loc6_20.4: init C = class_init (%.loc6_20.3), file.%c.var [template = constants.%struct]
// CHECK:STDOUT:   %.loc6_21: init C = converted %.loc6_20.1, %.loc6_20.4 [template = constants.%struct]
// CHECK:STDOUT:   assign file.%c.var, %.loc6_21
// CHECK:STDOUT:   return
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: --- use_both.carbon
// CHECK:STDOUT:
// CHECK:STDOUT: constants {
// CHECK:STDOUT:   %C: type = class_type @C [template]
// CHECK:STDOUT:   %.1: type = tuple_type () [template]
// CHECK:STDOUT:   %.2: type = struct_type {.x: ()} [template]
// CHECK:STDOUT:   %.3: type = ptr_type {.x: ()} [template]
// CHECK:STDOUT:   %tuple: () = tuple_value () [template]
// CHECK:STDOUT:   %struct: C = struct_value (%tuple) [template]
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: file {
// CHECK:STDOUT:   package: <namespace> = namespace [template] {
// CHECK:STDOUT:     .C = %import_ref.1
// CHECK:STDOUT:     .c = %c
// CHECK:STDOUT:   }
// CHECK:STDOUT:   %import_ref.1: type = import_ref ir1, inst+11, loaded [template = constants.%C]
// CHECK:STDOUT:   %import_ref.2 = import_ref ir1, inst+10, unloaded
// CHECK:STDOUT:   %import_ref.3 = import_ref ir1, inst+9, unloaded
// CHECK:STDOUT:   %C.ref: type = name_ref C, %import_ref.1 [template = constants.%C]
// CHECK:STDOUT:   %c.var: ref C = var c
// CHECK:STDOUT:   %c: ref C = bind_name c, %c.var
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: class @C {
// CHECK:STDOUT: !members:
// CHECK:STDOUT:   .Self = file.%import_ref.2
// CHECK:STDOUT:   .x = file.%import_ref.3
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: fn @__global_init() {
// CHECK:STDOUT: !entry:
// CHECK:STDOUT:   %.loc7_19.1: () = tuple_literal ()
// CHECK:STDOUT:   %.loc7_20.1: {.x: ()} = struct_literal (%.loc7_19.1)
// CHECK:STDOUT:   %.loc7_20.2: ref () = class_element_access file.%c.var, element0
// CHECK:STDOUT:   %.loc7_19.2: init () = tuple_init () to %.loc7_20.2 [template = constants.%tuple]
// CHECK:STDOUT:   %.loc7_20.3: init () = converted %.loc7_19.1, %.loc7_19.2 [template = constants.%tuple]
// CHECK:STDOUT:   %.loc7_20.4: init C = class_init (%.loc7_20.3), file.%c.var [template = constants.%struct]
// CHECK:STDOUT:   %.loc7_21: init C = converted %.loc7_20.1, %.loc7_20.4 [template = constants.%struct]
// CHECK:STDOUT:   assign file.%c.var, %.loc7_21
// CHECK:STDOUT:   return
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: --- fail_nonexport_use_both.carbon
// CHECK:STDOUT:
// CHECK:STDOUT: constants {
// CHECK:STDOUT:   %.1: type = tuple_type () [template]
// CHECK:STDOUT:   %.2: type = struct_type {.y: ()} [template]
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: file {
// CHECK:STDOUT:   package: <namespace> = namespace [template] {
// CHECK:STDOUT:     .C = %import_ref
// CHECK:STDOUT:     .d = %d
// CHECK:STDOUT:   }
// CHECK:STDOUT:   %import_ref = import_ref ir1, inst+11, unloaded
// CHECK:STDOUT:   %D.ref: <error> = name_ref D, <error> [template = <error>]
// CHECK:STDOUT:   %d.var: ref <error> = var d
// CHECK:STDOUT:   %d: ref <error> = bind_name d, %d.var
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: fn @__global_init() {
// CHECK:STDOUT: !entry:
// CHECK:STDOUT:   %.loc10_19: () = tuple_literal ()
// CHECK:STDOUT:   %.loc10_20: {.y: ()} = struct_literal (%.loc10_19)
// CHECK:STDOUT:   assign file.%d.var, <error>
// CHECK:STDOUT:   return
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: --- use_both_reversed.carbon
// CHECK:STDOUT:
// CHECK:STDOUT: constants {
// CHECK:STDOUT:   %C: type = class_type @C [template]
// CHECK:STDOUT:   %.1: type = tuple_type () [template]
// CHECK:STDOUT:   %.2: type = struct_type {.x: ()} [template]
// CHECK:STDOUT:   %.3: type = ptr_type {.x: ()} [template]
// CHECK:STDOUT:   %tuple: () = tuple_value () [template]
// CHECK:STDOUT:   %struct: C = struct_value (%tuple) [template]
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: file {
// CHECK:STDOUT:   package: <namespace> = namespace [template] {
// CHECK:STDOUT:     .C = %import_ref.1
// CHECK:STDOUT:     .c = %c
// CHECK:STDOUT:   }
// CHECK:STDOUT:   %import_ref.1: type = import_ref ir1, inst+11, loaded [template = constants.%C]
// CHECK:STDOUT:   %import_ref.2 = import_ref ir1, inst+10, unloaded
// CHECK:STDOUT:   %import_ref.3 = import_ref ir1, inst+9, unloaded
// CHECK:STDOUT:   %C.ref: type = name_ref C, %import_ref.1 [template = constants.%C]
// CHECK:STDOUT:   %c.var: ref C = var c
// CHECK:STDOUT:   %c: ref C = bind_name c, %c.var
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: class @C {
// CHECK:STDOUT: !members:
// CHECK:STDOUT:   .Self = file.%import_ref.2
// CHECK:STDOUT:   .x = file.%import_ref.3
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: fn @__global_init() {
// CHECK:STDOUT: !entry:
// CHECK:STDOUT:   %.loc7_19.1: () = tuple_literal ()
// CHECK:STDOUT:   %.loc7_20.1: {.x: ()} = struct_literal (%.loc7_19.1)
// CHECK:STDOUT:   %.loc7_20.2: ref () = class_element_access file.%c.var, element0
// CHECK:STDOUT:   %.loc7_19.2: init () = tuple_init () to %.loc7_20.2 [template = constants.%tuple]
// CHECK:STDOUT:   %.loc7_20.3: init () = converted %.loc7_19.1, %.loc7_19.2 [template = constants.%tuple]
// CHECK:STDOUT:   %.loc7_20.4: init C = class_init (%.loc7_20.3), file.%c.var [template = constants.%struct]
// CHECK:STDOUT:   %.loc7_21: init C = converted %.loc7_20.1, %.loc7_20.4 [template = constants.%struct]
// CHECK:STDOUT:   assign file.%c.var, %.loc7_21
// CHECK:STDOUT:   return
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: --- use_both_and_export_import.carbon
// CHECK:STDOUT:
// CHECK:STDOUT: constants {
// CHECK:STDOUT:   %C: type = class_type @C [template]
// CHECK:STDOUT:   %.1: type = tuple_type () [template]
// CHECK:STDOUT:   %.2: type = struct_type {.x: ()} [template]
// CHECK:STDOUT:   %.3: type = ptr_type {.x: ()} [template]
// CHECK:STDOUT:   %tuple: () = tuple_value () [template]
// CHECK:STDOUT:   %struct.1: C = struct_value (%tuple) [template]
// CHECK:STDOUT:   %D: type = class_type @D [template]
// CHECK:STDOUT:   %.4: type = struct_type {.y: ()} [template]
// CHECK:STDOUT:   %.5: type = ptr_type {.y: ()} [template]
// CHECK:STDOUT:   %struct.2: D = struct_value (%tuple) [template]
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: file {
// CHECK:STDOUT:   package: <namespace> = namespace [template] {
// CHECK:STDOUT:     .C = %import_ref.1
// CHECK:STDOUT:     .D = %import_ref.2
// CHECK:STDOUT:     .c = %c
// CHECK:STDOUT:     .d = %d
// CHECK:STDOUT:   }
// CHECK:STDOUT:   %import_ref.1: type = import_ref ir1, inst+11, loaded [template = constants.%C]
// CHECK:STDOUT:   %import_ref.2: type = import_ref ir4, inst+11, loaded [template = constants.%D]
// CHECK:STDOUT:   %import_ref.3 = import_ref ir1, inst+10, unloaded
// CHECK:STDOUT:   %import_ref.4 = import_ref ir1, inst+9, unloaded
// CHECK:STDOUT:   %C.ref: type = name_ref C, %import_ref.1 [template = constants.%C]
// CHECK:STDOUT:   %c.var: ref C = var c
// CHECK:STDOUT:   %c: ref C = bind_name c, %c.var
// CHECK:STDOUT:   %import_ref.5 = import_ref ir4, inst+12, unloaded
// CHECK:STDOUT:   %import_ref.6 = import_ref ir4, inst+16, unloaded
// CHECK:STDOUT:   %D.ref: type = name_ref D, %import_ref.2 [template = constants.%D]
// CHECK:STDOUT:   %d.var: ref D = var d
// CHECK:STDOUT:   %d: ref D = bind_name d, %d.var
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: class @C {
// CHECK:STDOUT: !members:
// CHECK:STDOUT:   .Self = file.%import_ref.3
// CHECK:STDOUT:   .x = file.%import_ref.4
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: class @D {
// CHECK:STDOUT: !members:
// CHECK:STDOUT:   .Self = file.%import_ref.5
// CHECK:STDOUT:   .y = file.%import_ref.6
// CHECK:STDOUT: }
// CHECK:STDOUT:
// CHECK:STDOUT: fn @__global_init() {
// CHECK:STDOUT: !entry:
// CHECK:STDOUT:   %.loc8_19.1: () = tuple_literal ()
// CHECK:STDOUT:   %.loc8_20.1: {.x: ()} = struct_literal (%.loc8_19.1)
// CHECK:STDOUT:   %.loc8_20.2: ref () = class_element_access file.%c.var, element0
// CHECK:STDOUT:   %.loc8_19.2: init () = tuple_init () to %.loc8_20.2 [template = constants.%tuple]
// CHECK:STDOUT:   %.loc8_20.3: init () = converted %.loc8_19.1, %.loc8_19.2 [template = constants.%tuple]
// CHECK:STDOUT:   %.loc8_20.4: init C = class_init (%.loc8_20.3), file.%c.var [template = constants.%struct.1]
// CHECK:STDOUT:   %.loc8_21: init C = converted %.loc8_20.1, %.loc8_20.4 [template = constants.%struct.1]
// CHECK:STDOUT:   assign file.%c.var, %.loc8_21
// CHECK:STDOUT:   %.loc9_19.1: () = tuple_literal ()
// CHECK:STDOUT:   %.loc9_20.1: {.y: ()} = struct_literal (%.loc9_19.1)
// CHECK:STDOUT:   %.loc9_20.2: ref () = class_element_access file.%d.var, element0
// CHECK:STDOUT:   %.loc9_19.2: init () = tuple_init () to %.loc9_20.2 [template = constants.%tuple]
// CHECK:STDOUT:   %.loc9_20.3: init () = converted %.loc9_19.1, %.loc9_19.2 [template = constants.%tuple]
// CHECK:STDOUT:   %.loc9_20.4: init D = class_init (%.loc9_20.3), file.%d.var [template = constants.%struct.2]
// CHECK:STDOUT:   %.loc9_21: init D = converted %.loc9_20.1, %.loc9_20.4 [template = constants.%struct.2]
// CHECK:STDOUT:   assign file.%d.var, %.loc9_21
// CHECK:STDOUT:   return
// CHECK:STDOUT: }
// CHECK:STDOUT:
