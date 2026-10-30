"""Exercise the production scratch ownership branches without a codec or device."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def block(source, start):
    begin = source.index('{', start)
    end, depth = begin + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class DecoderWorkspaceTests(unittest.TestCase):
    def test_profile_borrowing_and_generic_ownership(self):
        compiler = shutil.which(os.environ.get('CXX', 'g++'))
        if compiler is None:
            self.skipTest('A host C++ compiler is required')
        source = (ROOT / '3rd-party/fdk-aac/libAACdec/src/aacdecoder.cpp').read_text()
        opened = source.index('  self->workBufferCore1 = (FIXP_DBL *)GetWorkBufferCore1();')
        opened = source[opened:source.index('  self->workBufferCore2 =', opened)]
        deinit = source.index('static void CAacDecoder_DeInit(')
        release = block(source, source.index('          if (ch == aacChannelOffset)', deinit))
        start = source.index('#ifdef FDK_AIRPODS_PROFILE\n                /* The fixed ELD filterbank')
        bind = source[start:source.index('#endif', start) + len('#endif')]
        close = block(source, source.index('  if (self->workBufferCore1 != NULL)', deinit))
        harness = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <set>
typedef int32_t FIXP_DBL;
struct CWorkBufferCore1 { FIXP_DBL words[2048]; };
struct Common { CWorkBufferCore1 *pWorkBufferCore1; };
struct Channel { Common *pComStaticData; };
struct Decoder { FIXP_DBL *workBufferCore1; Channel *pAacDecoderChannelInfo[8]; };
static std::set<void *> live;
static unsigned allocations, releases;
static bool fail_next;
static CWorkBufferCore1 *GetWorkBufferCore1() {
    if (fail_next) { fail_next=false; return nullptr; }
    auto *p=static_cast<CWorkBufferCore1 *>(std::calloc(1,sizeof(CWorkBufferCore1)));
    assert(p && live.insert(p).second); ++allocations; return p;
}
static void FreeWorkBufferCore1(CWorkBufferCore1 **p) {
    assert(p && *p && live.erase(*p)==1); std::free(*p); *p=nullptr; ++releases;
}
static bool open_owner(Decoder *self) {
''' + opened + r'''
    return true;
#ifdef FDK_AIRPODS_PROFILE
bail:
    return false;
#endif
}
static void configure(Decoder *self) {
    const int ch=0;
''' + bind + r'''
    assert(self->pAacDecoderChannelInfo[ch]->pComStaticData->pWorkBufferCore1);
}
static void deconfigure(Decoder *self) {
    const int ch=0,aacChannelOffset=0;
''' + release + r'''
}
static void close_owner(Decoder *self) {
''' + close + r'''
}
int main() {
    Common common{}; Channel channel{&common}; Decoder state{};
    state.pAacDecoderChannelInfo[0]=&channel;
#ifdef FDK_AIRPODS_PROFILE
    fail_next=true;
    assert(!open_owner(&state) && !state.workBufferCore1 && live.empty());
    close_owner(&state); // Open failure cleanup has no allocated owner.
#endif
    for(unsigned cycle=0;cycle<64;++cycle) {
        unsigned before=allocations,freed=releases;
        assert(open_owner(&state)); auto *owner=state.workBufferCore1;
        for(unsigned configuration=0;configuration<8;++configuration) {
            configure(&state);
#ifdef FDK_AIRPODS_PROFILE
            assert(reinterpret_cast<void *>(common.pWorkBufferCore1)==owner);
            assert(allocations==before+1 && releases==freed);
#else
            assert(reinterpret_cast<void *>(common.pWorkBufferCore1)!=owner);
            assert(allocations==before+2+configuration);
            assert(releases==freed+configuration);
#endif
            deconfigure(&state); // Also the cleanup used after failed configuration.
            assert(!common.pWorkBufferCore1 && live.count(owner)==1);
            assert(state.workBufferCore1==owner);
        }
#ifdef FDK_AIRPODS_PROFILE
        common.pWorkBufferCore1=GetWorkBufferCore1();
        deconfigure(&state); // An unexpected separately owned buffer is still freed.
        assert(!common.pWorkBufferCore1 && live.count(owner)==1);
#endif
        close_owner(&state); assert(!state.workBufferCore1 && live.empty());
        close_owner(&state); // Repeated empty-owner cleanup never double-frees.
    }
    assert(allocations==releases);
}
'''
        with tempfile.TemporaryDirectory(prefix='decoder-workspace-') as temp:
            directory = pathlib.Path(temp)
            path = directory / 'workspace.cpp'
            path.write_text(harness)
            for profile in (False, True):
                binary = directory / ('profile' if profile else 'generic')
                flags = ['-DFDK_AIRPODS_PROFILE=1'] if profile else []
                subprocess.run([compiler, '-std=c++11', '-Wall', '-Wextra', '-Werror',
                                '-O2', *flags, str(path), '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
