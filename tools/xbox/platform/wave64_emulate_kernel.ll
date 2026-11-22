target datalayout = "e-m:e-p:32:32-i1:32-i8:8-i16:16-i32:32-i64:64-f16:16-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

%dx.types.Handle = type { i8* }
%dx.types.ResRet.i32 = type { i32, i32, i32, i32, i32 }
%struct.RWByteAddressBuffer = type { i32 }

define void @main() {
  %h0 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 0, i32 0, i1 false)
  %h1 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 1, i32 1, i1 false)
  %group = call i32 @dx.op.groupId.i32(i32 94, i32 0)
  %flat = call i32 @dx.op.flattenedThreadIdInGroup.i32(i32 96)
  %gbase = shl i32 %group, 6
  %thread = add i32 %gbase, %flat
  %in_off = shl i32 %thread, 4
  %in = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %h0, i32 %in_off, i32 undef, i8 3, i32 4)
  %x = extractvalue %dx.types.ResRet.i32 %in, 0
  %i = extractvalue %dx.types.ResRet.i32 %in, 1
  %f = sitofp i32 %i to float
  %sum = call i32 @dx.op.waveActiveOp.i32(i32 119, i32 %x, i8 0, i8 0)
  %min_s = call i32 @dx.op.waveActiveOp.i32(i32 119, i32 %x, i8 2, i8 1)
  %max_u = call i32 @dx.op.waveActiveOp.i32(i32 119, i32 %x, i8 3, i8 0)
  %prod = call i32 @dx.op.waveActiveOp.i32(i32 119, i32 %x, i8 1, i8 0)
  %or = call i32 @dx.op.waveActiveBit.i32(i32 120, i32 %x, i8 0)
  %and = call i32 @dx.op.waveActiveBit.i32(i32 120, i32 %x, i8 1)
  %xor = call i32 @dx.op.waveActiveBit.i32(i32 120, i32 %x, i8 2)
  %sumf = call float @dx.op.waveActiveOp.f32(i32 119, float %f, i8 0, i8 0)
  %firstf = call float @dx.op.waveReadLaneFirst.f32(i32 118, float %f)
  %lane = call i32 @dx.op.waveGetLaneIndex(i32 111)
  %pick = xor i32 %lane, 17
  %atf = call float @dx.op.waveReadLaneAt.f32(i32 117, float %f, i32 %pick)
  %sumf_bits = bitcast float %sumf to i32
  %firstf_bits = bitcast float %firstf to i32
  %atf_bits = bitcast float %atf to i32
  %wide_lo = zext i32 %x to i64
  %wide_hi = zext i32 %i to i64
  %wide_hi_shift = shl i64 %wide_hi, 32
  %wide = or i64 %wide_lo, %wide_hi_shift
  %first64 = call i64 @dx.op.waveReadLaneFirst.i64(i32 118, i64 %wide)
  %first64_lo = trunc i64 %first64 to i32
  %first64_hi_shift = lshr i64 %first64, 32
  %first64_hi = trunc i64 %first64_hi_shift to i32
  %quad = call i32 @dx.op.quadReadLaneAt.i32(i32 122, i32 %x, i32 2)
  %qopx = call float @dx.op.quadOp.f32(i32 123, float %f, i8 0)
  %qopd = call i32 @dx.op.quadOp.i32(i32 123, i32 %x, i8 2)
  %qopx_bits = bitcast float %qopx to i32
  %out_off = mul i32 %thread, 4
  %out0 = shl i32 %out_off, 4
  %out1 = add i32 %out0, 16
  %out2 = add i32 %out0, 32
  %out3 = add i32 %out0, 48
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %h1, i32 %out0, i32 undef, i32 %sum, i32 %min_s, i32 %max_u, i32 %prod, i8 15, i32 4)
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %h1, i32 %out1, i32 undef, i32 %or, i32 %and, i32 %xor, i32 %sumf_bits, i8 15, i32 4)
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %h1, i32 %out2, i32 undef, i32 %firstf_bits, i32 %atf_bits, i32 %first64_lo, i32 %first64_hi, i8 15, i32 4)
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %h1, i32 %out3, i32 undef, i32 %lane, i32 %quad, i32 %qopx_bits, i32 %qopd, i8 15, i32 4)
  ret void
}

declare %dx.types.Handle @dx.op.createHandle(i32, i8, i32, i32, i1) #2
declare i32 @dx.op.groupId.i32(i32, i32) #0
declare i32 @dx.op.flattenedThreadIdInGroup.i32(i32) #0
declare %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32, %dx.types.Handle, i32, i32, i8, i32) #2
declare void @dx.op.rawBufferStore.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i8, i32) #1
declare i32 @dx.op.waveActiveOp.i32(i32, i32, i8, i8) #1
declare i32 @dx.op.waveActiveBit.i32(i32, i32, i8) #1
declare float @dx.op.waveActiveOp.f32(i32, float, i8, i8) #1
declare float @dx.op.waveReadLaneFirst.f32(i32, float) #1
declare float @dx.op.waveReadLaneAt.f32(i32, float, i32) #1
declare i64 @dx.op.waveReadLaneFirst.i64(i32, i64) #1
declare i32 @dx.op.quadReadLaneAt.i32(i32, i32, i32) #1
declare float @dx.op.quadOp.f32(i32, float, i8) #1
declare i32 @dx.op.quadOp.i32(i32, i32, i8) #1
declare i32 @dx.op.waveGetLaneIndex(i32) #0
attributes #0 = { nounwind readnone }
attributes #1 = { nounwind }
attributes #2 = { nounwind readonly }
!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!2}
!dx.shaderModel = !{!3}
!dx.resources = !{!4}
!dx.entryPoints = !{!10}
!0 = !{!"dxc 1.8"}
!1 = !{i32 1, i32 5}
!2 = !{i32 1, i32 8}
!3 = !{!"cs", i32 6, i32 5}
!4 = !{null, !5, null, null}
!5 = !{!6, !7}
!6 = !{i32 0, %struct.RWByteAddressBuffer* undef, !"", i32 0, i32 0, i32 1, i32 11, i1 false, i1 false, i1 false, null}
!7 = !{i32 1, %struct.RWByteAddressBuffer* undef, !"", i32 0, i32 1, i32 1, i32 11, i1 false, i1 false, i1 false, null}
!10 = !{void ()* @main, !"main", null, !4, !11}
!11 = !{i32 0, i64 524304, i32 4, !12}
!12 = !{i32 64, i32 1, i32 1}
