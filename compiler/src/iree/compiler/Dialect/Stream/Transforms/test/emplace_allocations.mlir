// RUN: iree-opt --split-input-file --pass-pipeline='builtin.module(func.func(iree-stream-emplace-allocations))' %s | FileCheck %s

// Tests that a dispatch result is placed into the target of an update.

// CHECK-LABEL: @emplaceDispatch
func.func @emplaceDispatch(
    // CHECK-SAME: %[[INPUT:arg[0-9]+]]: !stream.resource<*>, %[[INPUT_SIZE:arg[0-9]+]]: index,
    %input: !stream.resource<*>, %input_size: index,
    // CHECK-SAME: %[[UPDATE_OFFSET:arg[0-9]+]]: index, %[[UPDATE_SIZE:arg[0-9]+]]: index,
    %update_offset: index, %update_size: index,
    // CHECK-SAME: %[[TARGET:arg[0-9]+]]: !stream.resource<*>, %[[TARGET_SIZE:arg[0-9]+]]: index
    %target: !stream.resource<*>, %target_size: index) -> !stream.resource<*> {
  %c0 = arith.constant 0 : index
  // CHECK-DAG: %[[UPDATE_END:.+]] = arith.addi %[[UPDATE_OFFSET]], %[[UPDATE_SIZE]]
  %update_end = arith.addi %update_offset, %update_size : index
  // CHECK: %[[RESULT:.+]] = stream.async.dispatch @ex::@dispatch(%[[INPUT]][{{.+}}], %[[TARGET]][%[[UPDATE_OFFSET]] to %[[UPDATE_END]] for %[[UPDATE_SIZE]]]) :
  // CHECK-SAME: (!stream.resource<*>{%[[INPUT_SIZE]]}, !stream.resource<*>{%[[TARGET_SIZE]]}) -> %[[TARGET]]{%[[TARGET_SIZE]]}
  %update = stream.async.dispatch @ex::@dispatch(%input[%c0 to %input_size for %input_size]) : (!stream.resource<*>{%input_size}) -> !stream.resource<*>{%update_size}
  // CHECK-NOT: stream.async.update
  %result = stream.async.update %update, %target[%update_offset to %update_end] : !stream.resource<*>{%update_size} -> %target as !stream.resource<*>{%target_size}
  // CHECK: return %[[RESULT]]
  return %result : !stream.resource<*>
}

// -----

// Tests that a dispatch with a tied result does not get placed. We could
// possibly untie the operand and place the result into the target resource but
// if the dispatch requires in-place operation that may not be safe.

// CHECK-LABEL: @dontEmplaceTiedDispatch
func.func @dontEmplaceTiedDispatch(
    %tied_input: !stream.resource<*>, %tied_input_size: index,
    %update_offset: index, %update_size: index,
    %target: !stream.resource<*>, %target_size: index) -> !stream.resource<*> {
  %c0 = arith.constant 0 : index
  %update_end = arith.addi %update_offset, %update_size : index
  // CHECK: %[[TIED_RESULT:.+]] = stream.async.dispatch @ex::@dispatch
  %update = stream.async.dispatch @ex::@dispatch(%tied_input[%c0 to %tied_input_size for %tied_input_size]) : (!stream.resource<*>{%tied_input_size}) -> %tied_input{%tied_input_size}
  // CHECK: %[[RESULT:.+]] = stream.async.update %[[TIED_RESULT]]
  %result = stream.async.update %update, %target[%update_offset to %update_end] : !stream.resource<*>{%tied_input_size} -> %target as !stream.resource<*>{%target_size}
  // CHECK: return %[[RESULT]]
  return %result : !stream.resource<*>
}

// -----

// Tests that sequences of updates get reordered and placed.
// This pattern originates from concats in higher level representations and we
// test that explicitly as it's 95% of what this pass is designed to optimize.

// CHECK-LABEL: @emplaceDispatchSequence
func.func @emplaceDispatchSequence(
    // CHECK-SAME: %[[INPUT:arg[0-9]+]]: !stream.resource<*>, %[[INPUT_SIZE:arg[0-9]+]]: index,
    %input: !stream.resource<*>, %input_size: index,
    // CHECK-SAME: %[[UPDATE_SIZE:arg[0-9]+]]: index, %[[TARGET_SIZE:arg[0-9]+]]: index
    %update_size: index, %target_size: index) -> !stream.resource<*> {
  %c0 = arith.constant 0 : index
  %c49152 = arith.constant 49152 : index
  %c98304 = arith.constant 98304 : index
  %c147456 = arith.constant 147456 : index
  %c196608 = arith.constant 196608 : index
  // CHECK: %[[TARGET:.+]] = stream.async.alloca
  // CHECK: %[[TARGET0:.+]] = stream.async.dispatch @ex::@dispatch0({{.+}}, %[[TARGET]][%c0 to %c49152 for %[[UPDATE_SIZE]]]) : ({{.+}}) -> %[[TARGET]]{%[[TARGET_SIZE]]}
  %update0 = stream.async.dispatch @ex::@dispatch0(%input[%c0 to %input_size for %input_size]) : (!stream.resource<*>{%input_size}) -> !stream.resource<*>{%update_size}
  // CHECK: %[[TARGET1:.+]] = stream.async.dispatch @ex::@dispatch1({{.+}}, %[[TARGET0]][%c49152 to %c98304 for %[[UPDATE_SIZE]]]) : ({{.+}}) -> %[[TARGET0]]{%[[TARGET_SIZE]]}
  %update1 = stream.async.dispatch @ex::@dispatch1(%input[%c0 to %input_size for %input_size]) : (!stream.resource<*>{%input_size}) -> !stream.resource<*>{%update_size}
  // CHECK: %[[TARGET2:.+]] = stream.async.dispatch @ex::@dispatch2({{.+}}, %[[TARGET1]][%c98304 to %c147456 for %[[UPDATE_SIZE]]]) : ({{.+}}) -> %[[TARGET1]]{%[[TARGET_SIZE]]}
  %update2 = stream.async.dispatch @ex::@dispatch2(%input[%c0 to %input_size for %input_size]) : (!stream.resource<*>{%input_size}) -> !stream.resource<*>{%update_size}
  // CHECK: %[[TARGET3:.+]] = stream.async.dispatch @ex::@dispatch3({{.+}}, %[[TARGET2]][%c147456 to %c196608 for %[[UPDATE_SIZE]]]) : ({{.+}}) -> %[[TARGET2]]{%[[TARGET_SIZE]]}
  %update3 = stream.async.dispatch @ex::@dispatch3(%input[%c0 to %input_size for %input_size]) : (!stream.resource<*>{%input_size}) -> !stream.resource<*>{%update_size}
  // CHECK-NOT: stream.async.alloca
  %target = stream.async.alloca : !stream.resource<*>{%target_size}
  // CHECK-NOT: stream.async.update
  %target0 = stream.async.update %update0, %target[%c0 to %c49152] : !stream.resource<*>{%update_size} -> %target as !stream.resource<*>{%target_size}
  // CHECK-NOT: stream.async.update
  %target1 = stream.async.update %update1, %target0[%c49152 to %c98304] : !stream.resource<*>{%update_size} -> %target0 as !stream.resource<*>{%target_size}
  // CHECK-NOT: stream.async.update
  %target2 = stream.async.update %update2, %target1[%c98304 to %c147456] : !stream.resource<*>{%update_size} -> %target1 as !stream.resource<*>{%target_size}
  // CHECK-NOT: stream.async.update
  %target3 = stream.async.update %update3, %target2[%c147456 to %c196608] : !stream.resource<*>{%update_size} -> %target2 as !stream.resource<*>{%target_size}
  // CHECK: return %[[TARGET3]]
  return %target3 : !stream.resource<*>
}

// -----

// Tests a concat-like sequence that has some inter-dependencies - these
// dependencies shouldn't stop us from emplacing.

// CHECK-LABEL: @emplaceMultiResultDispatchSequence
func.func @emplaceMultiResultDispatchSequence(
    // CHECK-SAME: %[[INPUT:arg[0-9]+]]: !stream.resource<*>, %[[INPUT_SIZE:arg[0-9]+]]: index,
    %input: !stream.resource<*>, %input_size: index,
    // CHECK-SAME: %[[UPDATE_SIZE:arg[0-9]+]]: index, %[[TARGET_SIZE:arg[0-9]+]]: index
    %update_size: index, %target_size: index) -> !stream.resource<*> {
  %c0 = arith.constant 0 : index
  %c49152 = arith.constant 49152 : index
  %c98304 = arith.constant 98304 : index
  %c147456 = arith.constant 147456 : index
  %c196608 = arith.constant 196608 : index
  // CHECK: %[[TARGET:.+]] = stream.async.alloca
  // CHECK: %[[TARGET0:.+]] = stream.async.dispatch @ex::@dispatch0({{.+}}, %[[TARGET]][%c0 to %c49152 for %[[UPDATE_SIZE]]]) : ({{.+}}) -> %[[TARGET]]{%[[TARGET_SIZE]]}
  %update0 = stream.async.dispatch @ex::@dispatch0(%input[%c0 to %input_size for %input_size]) : (!stream.resource<*>{%input_size}) -> !stream.resource<*>{%update_size}
  // CHECK: %[[TARGET1_TEMP:.+]]:2 = stream.async.dispatch @ex::@dispatch1({{.+}}, %[[TARGET0]][%c49152 to %c98304 for %[[UPDATE_SIZE]]]) : ({{.+}}) -> (%[[TARGET0]]{%[[TARGET_SIZE]]}, !stream.resource<*>{%[[INPUT_SIZE]]})
  %update1_temp:2 = stream.async.dispatch @ex::@dispatch1(%input[%c0 to %input_size for %input_size]) : (!stream.resource<*>{%input_size}) -> (!stream.resource<*>{%update_size}, !stream.resource<*>{%input_size})
  // CHECK: %[[TARGET2:.+]] = stream.async.dispatch @ex::@dispatch2({{.+}}, %[[TARGET1_TEMP]]#0[%c98304 to %c147456 for %[[UPDATE_SIZE]]]) : ({{.+}}) -> %[[TARGET1_TEMP]]#0{%[[TARGET_SIZE]]}
  %update2 = stream.async.dispatch @ex::@dispatch2(%update1_temp#1[%c0 to %input_size for %input_size]) : (!stream.resource<*>{%input_size}) -> !stream.resource<*>{%update_size}
  // CHECK: %[[TARGET3:.+]] = stream.async.dispatch @ex::@dispatch3({{.+}}, %[[TARGET2]][%c147456 to %c196608 for %[[UPDATE_SIZE]]]) : ({{.+}}) -> %[[TARGET2]]{%[[TARGET_SIZE]]}
  %update3 = stream.async.dispatch @ex::@dispatch3(%input[%c0 to %input_size for %input_size]) : (!stream.resource<*>{%input_size}) -> !stream.resource<*>{%update_size}
  // CHECK-NOT: stream.async.alloca
  %target = stream.async.alloca : !stream.resource<*>{%target_size}
  // CHECK-NOT: stream.async.update
  %target0 = stream.async.update %update0, %target[%c0 to %c49152] : !stream.resource<*>{%update_size} -> %target as !stream.resource<*>{%target_size}
  // CHECK-NOT: stream.async.update
  %target1 = stream.async.update %update1_temp#0, %target0[%c49152 to %c98304] : !stream.resource<*>{%update_size} -> %target0 as !stream.resource<*>{%target_size}
  // CHECK-NOT: stream.async.update
  %target2 = stream.async.update %update2, %target1[%c98304 to %c147456] : !stream.resource<*>{%update_size} -> %target1 as !stream.resource<*>{%target_size}
  // CHECK-NOT: stream.async.update
  %target3 = stream.async.update %update3, %target2[%c147456 to %c196608] : !stream.resource<*>{%update_size} -> %target2 as !stream.resource<*>{%target_size}
  // CHECK: return %[[TARGET3]]
  return %target3 : !stream.resource<*>
}

// -----

// Tests that multiple results from the same dispatch routing to the same target
// get placed. This can originate from concats where all producers are fused
// into the same dispatch.

// CHECK-LABEL: @emplaceMultiResultDispatchInto
func.func @emplaceMultiResultDispatchInto(
    // CHECK-SAME: %[[INPUT:arg[0-9]+]]: !stream.resource<*>, %[[INPUT_SIZE:arg[0-9]+]]: index,
    %input: !stream.resource<*>, %input_size: index,
    // CHECK-SAME: %[[UPDATE_SIZE:arg[0-9]+]]: index, %[[TARGET_SIZE:arg[0-9]+]]: index
    %update_size: index, %target_size: index) -> !stream.resource<*> {
  %c0 = arith.constant 0 : index
  %c32 = arith.constant 32 : index
  %c64 = arith.constant 64 : index
  // CHECK: %[[TARGET:.+]] = stream.async.alloca
  // CHECK: %[[DISPATCH:.+]]:2 = stream.async.dispatch @ex::@dispatch0
  // CHECK-SAME: ({{.+}}, %[[TARGET]][%c0 to %c32 for %[[UPDATE_SIZE]]], %[[TARGET]][%c32 to %c64 for %[[UPDATE_SIZE]]]) :
  // CHECK-SAME: ({{.+}}) -> (%[[TARGET]]{%[[TARGET_SIZE]]}, %[[TARGET]]{%[[TARGET_SIZE]]})
  %update:2 = stream.async.dispatch @ex::@dispatch0(%input[%c0 to %input_size for %input_size]) :
      (!stream.resource<*>{%input_size}) -> (!stream.resource<*>{%update_size}, !stream.resource<*>{%update_size})
  // CHECK-NOT: stream.async.alloca
  %target = stream.async.alloca : !stream.resource<*>{%target_size}
  // CHECK-NOT: stream.async.update
  %target0 = stream.async.update %update#0, %target[%c0 to %c32] : !stream.resource<*>{%update_size} -> %target as !stream.resource<*>{%target_size}
  // CHECK-NOT: stream.async.update
  %target1 = stream.async.update %update#1, %target0[%c32 to %c64] : !stream.resource<*>{%update_size} -> %target0 as !stream.resource<*>{%target_size}
  // CHECK: return %[[DISPATCH]]#1
  return %target1 : !stream.resource<*>
}

// -----

// Tests that sequences with data dependencies don't hoist beyond them.

// CHECK-LABEL: @emplaceDependentDispatchSequence
func.func @emplaceDependentDispatchSequence(
    // CHECK-SAME: %[[INPUT:arg[0-9]+]]: !stream.resource<*>, %[[INPUT_SIZE:arg[0-9]+]]: index,
    %input: !stream.resource<*>, %input_size: index,
    // CHECK-SAME: %[[UPDATE_SIZE:arg[0-9]+]]: index, %[[TARGET_SIZE:arg[0-9]+]]: index
    %update_size: index, %target_size: index) -> !stream.resource<*> {
  %c0 = arith.constant 0 : index
  %c49152 = arith.constant 49152 : index
  %c98304 = arith.constant 98304 : index
  // CHECK: stream.async.alloca
  // CHECK-NEXT: %[[TARGET0:.+]] = stream.async.dispatch @ex::@dispatch0
  %update0 = stream.async.dispatch @ex::@dispatch0(%input[%c0 to %input_size for %input_size]) : (!stream.resource<*>{%input_size}) -> !stream.resource<*>{%update_size}
  // CHECK-NEXT: %[[UPDATE1_ORIGIN:.+]] = stream.async.dispatch @ex::@dispatch1
  %update1_origin = stream.async.dispatch @ex::@dispatch1(%input[%c0 to %input_size for %input_size]) : (!stream.resource<*>{%input_size}) -> !stream.resource<*>{%update_size}
  // CHECK-NEXT: %[[UPDATE1:.+]] = util.optimization_barrier %[[UPDATE1_ORIGIN]]
  %update1 = util.optimization_barrier %update1_origin : !stream.resource<*>
  // CHECK-NOT: stream.async.alloca
  %target = stream.async.alloca : !stream.resource<*>{%target_size}
  // CHECK-NOT: stream.async.update
  %target0 = stream.async.update %update0, %target[%c0 to %c49152] : !stream.resource<*>{%update_size} -> %target as !stream.resource<*>{%target_size}
  // CHECK-NEXT: %[[TARGET1:.+]] = stream.async.update %[[UPDATE1]], %[[TARGET0]]
  %target1 = stream.async.update %update1, %target0[%c49152 to %c98304] : !stream.resource<*>{%update_size} -> %target0 as !stream.resource<*>{%target_size}
  // CHECK-NEXT: return %[[TARGET1]]
  return %target1 : !stream.resource<*>
}
