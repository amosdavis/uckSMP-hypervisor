//go:build !linux

package helpers

import (
	"fmt"
	"net"
	"os"
	"os/exec"
	"strings"
	"time"
)

const (
	UCK_DEVICE       = "/dev/uck"
	UCK_IOC_MAGIC    = 'U'
	UCK_MAX_NODES    = 16
	UCK_MAX_REGIONS  = 64
	UCK_PORT_DEFAULT = 9999
)

// Ioctl command numbers matching uck.h
const (
	UCK_IOC_SET_NODE      = 0x40104501
	UCK_IOC_ADD_NODE      = 0x40104502
	UCK_IOC_CREATE_REGION = 0x40184503
	UCK_IOC_JOIN_REGION   = 0x40184504
	UCK_IOC_GET_STATUS    = 0x80104505
	UCK_IOC_MIGRATE_PROC  = 0x40084506
	UCK_IOC_GET_CLUSTER   = 0x80184507
	UCK_IOC_REMOTE_EXEC   = 0xC3044508
	UCK_IOC_GET_JOBS      = 0xC38C4509
	UCK_IOC_ENABLE_SMP    = 0x4008450A
	UCK_IOC_FUTEX         = 0xC018450B
	UCK_IOC_NODE_LEAVE    = 0x0000450C
	UCK_IOC_GET_CGROUP    = 0x8020450D
)

// NodeInfo mirrors struct uck_node_info
type NodeInfo struct {
	NodeID uint32
	IPAddr uint32
	Port   uint16
	Flags  uint16
}

// ClusterInfo mirrors struct uck_cluster_info
type ClusterInfo struct {
	NumNodes     uint32
	TotalCPUs    uint32
	TotalMem     uint64
	TotalFree    uint64
	TotalRunning uint32
}

// MigrateReq mirrors struct uck_migrate_req
type MigrateReq struct {
	PID      uint32
	DestNode uint32
}

// RegionInfo mirrors struct uck_region_info
type RegionInfo struct {
	RegionID  uint64
	Size      uint64
	OwnerNode uint32
}

// UCKDevice wraps the /dev/uck file descriptor
type UCKDevice struct {
	fd int
}

// OpenUCK opens /dev/uck (Linux only, stub on other platforms)
func OpenUCK() (*UCKDevice, error) {
	return nil, fmt.Errorf("uck device is only available on Linux")
}

// Close closes the device
func (d *UCKDevice) Close() error {
	return fmt.Errorf("uck device is only available on Linux")
}

// SetNode sets the local node identity
func (d *UCKDevice) SetNode(info *NodeInfo) error {
	return fmt.Errorf("uck device is only available on Linux")
}

// GetCluster returns cluster info
func (d *UCKDevice) GetCluster() (*ClusterInfo, error) {
	return nil, fmt.Errorf("uck device is only available on Linux")
}

// IPToUint32 converts an IP string to network-byte-order uint32
func IPToUint32(ip string) uint32 {
	parsed := net.ParseIP(ip).To4()
	if parsed == nil {
		return 0
	}
	return (uint32(parsed[0]) << 24) | (uint32(parsed[1]) << 16) | (uint32(parsed[2]) << 8) | uint32(parsed[3])
}

// IsModuleLoaded checks if uck.ko is loaded (always false on non-Linux)
func IsModuleLoaded() bool {
	return false
}

// ReadProcFile reads a /proc/uck/ file (not available on non-Linux)
func ReadProcFile(name string) (string, error) {
	return "", fmt.Errorf("/proc/uck is only available on Linux")
}

// WaitForPort waits for a TCP port to be listening
func WaitForPort(host string, port int, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		conn, err := net.DialTimeout("tcp", fmt.Sprintf("%s:%d", host, port), time.Second)
		if err == nil {
			conn.Close()
			return nil
		}
		time.Sleep(500 * time.Millisecond)
	}
	return fmt.Errorf("timeout waiting for %s:%d", host, port)
}

// RunCommand runs a command and returns stdout
func RunCommand(name string, args ...string) (string, error) {
	cmd := exec.Command(name, args...)
	out, err := cmd.CombinedOutput()
	return string(out), err
}

// CheckDmesg searches kernel log for a pattern (not available on non-Linux)
func CheckDmesg(pattern string) (bool, error) {
	return false, fmt.Errorf("dmesg is only available on Linux")
}

// FileExists checks if a file exists
func FileExists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

// TCPSend sends raw bytes to a host:port
func TCPSend(host string, port int, data []byte) error {
	conn, err := net.DialTimeout("tcp", fmt.Sprintf("%s:%d", host, port), 5*time.Second)
	if err != nil {
		return err
	}
	defer conn.Close()
	_, err = conn.Write(data)
	return err
}

// TCPSendRecv sends data and receives a response
func TCPSendRecv(host string, port int, data []byte, respLen int) ([]byte, error) {
	conn, err := net.DialTimeout("tcp", fmt.Sprintf("%s:%d", host, port), 5*time.Second)
	if err != nil {
		return nil, err
	}
	defer conn.Close()
	_, err = conn.Write(data)
	if err != nil {
		return nil, err
	}
	resp := make([]byte, respLen)
	n, err := conn.Read(resp)
	if err != nil {
		return nil, err
	}
	return resp[:n], nil
}

// suppress unused import warnings
var _ = strings.Contains
