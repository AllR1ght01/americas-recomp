target datalayout = "e-m:e-p:32:32-i1:32-i8:8-i16:16-i32:32-i64:64-f16:16-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-xb2"

%dx.types.Handle = type { i8* }

define void @main() {
  %1 = call i32 @dx.op.groupId.i32(i32 94, i32 0)
  %2 = call i32 @dx.op.flattenedThreadIdInGroup.i32(i32 96)
  %3 = shl i32 %1, 6
  %4 = add i32 %3, %2
  %5 = zext i32 %4 to i64
  %6 = shl i64 %5, 2
  %7 = add i64 %6, 20015998304256
  %8 = call i32 @xbox.global_load_dword_with_cache(i32 356, i64 %7, i32 undef, i32 undef)
  %9 = shl i64 %5, 3
  %10 = add i64 %9, 20015998304256
  %11 = call { i32, i32 } @xbox.global_load_dwordx2_with_cache(i32 357, i64 %10, i32 undef, i32 undef)
  %12 = extractvalue { i32, i32 } %11, 0
  %13 = extractvalue { i32, i32 } %11, 1
  %14 = mul i64 %5, 12
  %15 = add i64 %14, 20015998304256
  %16 = call { i32, i32, i32 } @xbox.global_load_dwordx3_with_cache(i32 358, i64 %15, i32 undef, i32 undef)
  %17 = extractvalue { i32, i32, i32 } %16, 0
  %18 = extractvalue { i32, i32, i32 } %16, 1
  %19 = extractvalue { i32, i32, i32 } %16, 2
  %20 = shl i64 %5, 4
  %21 = add i64 %20, 20015998304256
  %22 = call { i32, i32, i32, i32 } @xbox.global_load_dwordx4_with_cache(i32 359, i64 %21, i32 undef, i32 undef)
  %23 = extractvalue { i32, i32, i32, i32 } %22, 0
  %24 = extractvalue { i32, i32, i32, i32 } %22, 1
  %25 = extractvalue { i32, i32, i32, i32 } %22, 2
  %26 = extractvalue { i32, i32, i32, i32 } %22, 3
  %27 = zext i32 %1 to i64
  %28 = shl i64 %27, 8
  %29 = add i64 %28, 20015998304256
  %30 = call i32 @xbox.s_load_dword_with_cache(i32 360, i64 %29, i32 undef, i32 undef)
  %31 = shl i64 %5, 6
  %32 = add i64 %31, 8589930496
  %33 = insertvalue { i32, i32, i32, i32 } undef, i32 %8, 0
  %34 = insertvalue { i32, i32, i32, i32 } %33, i32 %12, 1
  %35 = insertvalue { i32, i32, i32, i32 } %34, i32 %13, 2
  %36 = insertvalue { i32, i32, i32, i32 } %35, i32 %17, 3
  call void @xbox.global_store_dwordx4_with_cache(i32 363, i64 %32, { i32, i32, i32, i32 } %36, i32 undef, i32 undef)
  %37 = add i64 %32, 16
  %38 = insertvalue { i32, i32, i32 } undef, i32 %18, 0
  %39 = insertvalue { i32, i32, i32 } %38, i32 %19, 1
  %40 = insertvalue { i32, i32, i32 } %39, i32 %23, 2
  call void @xbox.global_store_dwordx3_with_cache(i32 362, i64 %37, { i32, i32, i32 } %40, i32 undef, i32 undef)
  %41 = add i64 %32, 28
  call void @xbox.global_store_dword_with_cache(i32 361, i64 %41, i32 %24, i32 undef, i32 undef)
  %42 = add i64 %32, 32
  %43 = insertvalue { i32, i32, i32, i32 } { i32 undef, i32 undef, i32 undef, i32 -8388609 }, i32 %25, 0
  %44 = insertvalue { i32, i32, i32, i32 } %43, i32 %26, 1
  %45 = insertvalue { i32, i32, i32, i32 } %44, i32 %30, 2
  call void @xbox.global_store_dwordx4_with_cache(i32 363, i64 %42, { i32, i32, i32, i32 } %45, i32 undef, i32 undef)
  %46 = and i32 %4, 15
  %47 = zext i32 %46 to i64
  %48 = shl i64 %47, 2
  %49 = add i64 %48, 8590979072
  %50 = call i32 @xbox.global_atomic_add(i32 364, i64 %49, i32 1)
  %51 = add i64 %49, 64
  %52 = call i32 @xbox.global_atomic_sub(i32 365, i64 %51, i32 2)
  %53 = ashr i32 %8, 8
  %54 = sitofp i32 %53 to float
  %55 = and i32 %4, 7
  %56 = zext i32 %55 to i64
  %57 = shl i64 %56, 2
  %58 = add i64 %57, 8590979200
  %59 = call float @xbox.global_atomic_fmin(i32 366, i64 %58, float %54)
  %60 = add i64 %58, 32
  %61 = call float @xbox.global_atomic_fmax(i32 367, i64 %60, float %54)
  ret void
}

declare i32 @dx.op.groupId.i32(i32, i32) #0
declare i32 @dx.op.flattenedThreadIdInGroup.i32(i32) #0
declare i32 @xbox.global_load_dword_with_cache(i32, i64, i32, i32) #2
declare { i32, i32 } @xbox.global_load_dwordx2_with_cache(i32, i64, i32, i32) #2
declare { i32, i32, i32 } @xbox.global_load_dwordx3_with_cache(i32, i64, i32, i32) #2
declare { i32, i32, i32, i32 } @xbox.global_load_dwordx4_with_cache(i32, i64, i32, i32) #2
declare i32 @xbox.s_load_dword_with_cache(i32, i64, i32, i32) #2
declare void @xbox.global_store_dword_with_cache(i32, i64, i32, i32, i32) #1
declare void @xbox.global_store_dwordx3_with_cache(i32, i64, { i32, i32, i32 }, i32, i32) #1
declare void @xbox.global_store_dwordx4_with_cache(i32, i64, { i32, i32, i32, i32 }, i32, i32) #1
declare i32 @xbox.global_atomic_add(i32, i64, i32) #1
declare i32 @xbox.global_atomic_sub(i32, i64, i32) #1
declare float @xbox.global_atomic_fmin(i32, i64, float) #1
declare float @xbox.global_atomic_fmax(i32, i64, float) #1
attributes #0 = { nounwind readnone }
attributes #1 = { nounwind }
attributes #2 = { nounwind readonly }
!xbox.compiler.options = !{!0}
!llvm.ident = !{!1}
!dx.version = !{!2}
!dx.valver = !{!3}
!dx.shaderModel = !{!4}
!dx.entryPoints = !{!5}
!0 = !{!"__XBOX_INLINE_SOURCE", !"1"}
!1 = !{!"dxcxbox 2504.2509.11601.10011"}
!2 = !{i32 1, i32 5}
!3 = !{i32 1, i32 8}
!4 = !{!"cs", i32 6, i32 5}
!5 = !{void ()* @main, !"main", null, null, !6}
!6 = !{i32 0, i64 1048576, i32 4, !7}
!7 = !{i32 64, i32 1, i32 1}
