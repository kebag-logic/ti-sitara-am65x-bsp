# The entity as a la_avdecc controller sees it

`milan_compat.cpp` is a controller on L-Acoustics' la_avdecc library (4.3.x,
<https://github.com/L-Acoustics/avdecc>), the library Milan controllers are
built on. It discovers one entity over pcap, enumerates it with the library's
full static enumeration (every descriptor read), and keeps listening while
unsolicited notifications and counters arrive. It prints every compatibility
event the library records, with the clause it cites, and every warning, error
and compatibility item the library logs. It passes only if the entity keeps
`IEEE17221` and `Milan`, gains none of `IEEE17221Warning`, `MilanWarning` or
`Misbehaving`, keeps the Milan version GET_MILAN_INFO gave, and nothing was
logged.

`run-netns-compat.sh` runs it against `milan-ctrld` and `milan-mediad` (talker,
AAF listener and CRF input, on a simulated USB host) in a private network
namespace, with `gptp.py` playing flexptpd. With `streams`, a bridge joins
the entity, the controller and `talker.py`, which binds STREAM_INPUT 0 to an
AAF stream and STREAM_INPUT 1 to a CRF stream it sends, so the controller
enumerates inputs that are bound and whose counters move.

Build la_avdecc, then the program:

    git clone --recursive --branch v4.3.1.1 https://github.com/L-Acoustics/avdecc SRC
    cmake -S SRC -B BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_AVDECC_LIB_SHARED_CXX=OFF
    ninja -C BUILD la_avdecc_controller_static
    make MILAN_FPGA=... LA_AVDECC_SRC=SRC LA_AVDECC_BUILD=BUILD check-compat

The library's own defaults turn on five leniencies (`IGNORE_INVALID_CONTROL_DATA_LENGTH`,
`IGNORE_INVALID_NON_SUCCESS_AEM_RESPONSES`, `IGNORE_NEITHER_STATIC_NOR_DYNAMIC_MAPPINGS`,
`CONTINUE_MISBEHAVE_AEM_RESPONSES`, `ALLOW_GET_AUDIO_MAP_UNSOL`), which a
controller built from it inherits. Configured with all five `OFF`, the library
judges strictly. The program prints the options it was compiled with.
