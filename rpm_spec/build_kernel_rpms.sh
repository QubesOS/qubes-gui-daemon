echo "**** Building for target: i686 ****"
for dir in /usr/src/kernels/*.i686*
do
    ! [ -d $dir ] && continue
    krnl=`basename $dir`
    echo Building rpm for kernel version $krnl...
    echo =========================================================================
    rpmbuild --target i686 --define "_target_cpu i686" --define "_rpmdir rpm/" --define "kernel_version $krnl" -bb rpm_spec/vchan-vm.spec || \
    	echo WARNING: BUILD FAILD!!!!!!!!!
done

echo "**** Building for target: x86_64 ****"
for dir in /usr/src/kernels/*.x86_64*
do
    ! [ -d $dir ] && continue
    krnl=`basename $dir`
    echo Building rpm for kernel version $krnl...
    echo =========================================================================
    rpmbuild --target x86_64 --define "_rpmdir rpm/" --define "kernel_version $krnl" -bb rpm_spec/vchan-vm.spec || \
    	echo WARNING: BUILD FAILD!!!!!!!!!
done

