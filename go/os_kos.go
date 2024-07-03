//go:build unknown

package runtime

type mOS struct{}

const (
	_PROT_NONE  = 0
	_PROT_READ  = 1
	_PROT_WRITE = 2

	_MAP_SHARED    = 0x0001
	_MAP_PRIVATE   = 0x0002
	_MAP_ANONYMOUS = 0x1000
	_MAP_FIXED     = 0x0010
)

func osinit() {
	ncpu = 4
	physPageSize = 4096
}

//go:noescape
//extern sem_init
func sem_init(mp *semaphore_t, count int32) int32

//go:noescape
//extern sem_wait
func sem_wait(sem *semaphore_t) int32

//go:noescape
//extern sem_signal
func sem_signal(sem *semt) int32

//go:nosplit
func semacreate(mp *m) {}

//go:nosplit
func semasleep(ns int64) int32 {
	return 0
}

//go:nosplit
func semawakeup(mp *m) {}

//go:nosplit
func setProcessCPUProfiler(hz int32) {
	// fuck you
}

//go:nosplit
func setThreadCPUProfiler(hz int32) {
	// fuck you
}

//go:nosplit
func validSIGPROF(mp *m, c *sigctxt) bool {
	return true
}
