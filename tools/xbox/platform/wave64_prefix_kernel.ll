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
  %count = call i32 @dx.op.waveGetLaneCount(i32 112)
  %first = call i1 @dx.op.waveIsFirstLane(i32 110)
  %first_i = zext i1 %first to i32
  %ps = call i32 @dx.op.wavePrefixOp.i32(i32 121, i32 %x, i8 0, i8 0)
  %pm = call i32 @dx.op.wavePrefixOp.i32(i32 121, i32 %x, i8 2, i8 1)
  %px = call i32 @dx.op.wavePrefixOp.i32(i32 121, i32 %x, i8 3, i8 0)
  %pf = call float @dx.op.wavePrefixOp.f32(i32 121, float %f, i8 0, i8 0)
  %pp = call i32 @dx.op.wavePrefixOp.i32(i32 121, i32 %x, i8 1, i8 0)
  %one = and i32 %x, 1
  %pred = icmp ne i32 %one, 0
  %any = call i1 @dx.op.waveAnyTrue(i32 113, i1 %pred)
  %all = call i1 @dx.op.waveAllTrue(i32 114, i1 %pred)
  %equal = call i1 @dx.op.waveActiveAllEqual.i32(i32 115, i32 %one)
  %pf_bits = bitcast float %pf to i32
  %any_i = zext i1 %any to i32
  %all_i = zext i1 %all to i32
  %equal_i = zext i1 %equal to i32
  %lane = call i32 @dx.op.waveGetLaneIndex(i32 111)
  %out_off = mul i32 %thread, 3
  %out0 = shl i32 %out_off, 4
  %out1 = add i32 %out0, 16
  %out2 = add i32 %out0, 32
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %h1, i32 %out0, i32 undef, i32 %count, i32 %first_i, i32 %ps, i32 %pm, i8 15, i32 4)
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %h1, i32 %out1, i32 undef, i32 %px, i32 %pf_bits, i32 %pp, i32 %any_i, i8 15, i32 4)
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %h1, i32 %out2, i32 undef, i32 %all_i, i32 %equal_i, i32 %lane, i32 undef, i8 7, i32 4)
  ret void
}

declare %dx.types.Handle @dx.op.createHandle(i32, i8, i32, i32, i1) #2
declare i32 @dx.op.groupId.i32(i32, i32) #0
declare i32 @dx.op.flattenedThreadIdInGroup.i32(i32) #0
declare %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32, %dx.types.Handle, i32, i32, i8, i32) #2
declare void @dx.op.rawBufferStore.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i8, i32) #1
declare i32 @dx.op.waveGetLaneCount(i32) #0
declare i1 @dx.op.waveIsFirstLane(i32) #1
declare i32 @dx.op.wavePrefixOp.i32(i32, i32, i8, i8) #1
declare float @dx.op.wavePrefixOp.f32(i32, float, i8, i8) #1
declare i1 @dx.op.waveAnyTrue(i32, i1) #1
declare i1 @dx.op.waveAllTrue(i32, i1) #1
declare i1 @dx.op.waveActiveAllEqual.i32(i32, i32) #1
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
