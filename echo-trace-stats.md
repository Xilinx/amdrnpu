# tessai-agent echo test latency statistics

Runs analyzed: **1000**  
Excluded steps: sdt_tessai_agent:cmd_submit_exit, sdt_tessai_agent:wait_ioctl_enter, tessai_agent:tessai_cmd_submit_exit

## Per-step summary (microseconds)

| Step | Event | Count | Min | Max | Avg | Std |
|-----:|-------|------:|----:|----:|----:|----:|
| 0 | `sdt_tessai_agent:cmd_submit_enter -> tessai_agent:tessai_cmd_submit_enter` | 1000 | 1.000 | 18.000 | 1.587 | 0.723 |
| 1 | `tessai_agent:tessai_cmd_submit_enter -> tessai_agent:tessai_rpmsg_send_enter` | 1000 | 0.000 | 8.000 | 1.266 | 0.915 |
| 2 | `tessai_agent:tessai_rpmsg_send_enter -> zynqmp_ipi:zynqmp_ipi_send(TX)` | 1000 | 3.000 | 17.000 | 4.056 | 0.568 |
| 3 | `zynqmp_ipi:zynqmp_ipi_send(TX) -> tessai_agent:tessai_rpmsg_send_exit` | 1000 | 0.000 | 2.000 | 1.069 | 0.269 |
| 4 | `tessai_agent:tessai_rpmsg_send_exit -> zynqmp_ipi_gic:zynqmp_ipi_gic_entry` | 1000 | 89.000 | 121.000 | 101.740 | 1.944 |
| 5 | `zynqmp_ipi_gic:zynqmp_ipi_gic_entry -> zynqmp_ipi:zynqmp_ipi_isr` | 1000 | 0.000 | 4.000 | 0.371 | 0.496 |
| 6 | `zynqmp_ipi:zynqmp_ipi_isr -> zynqmp_ipi:zynqmp_ipi_send(RX)` | 1000 | 4.000 | 10.000 | 5.014 | 0.272 |
| 7 | `zynqmp_ipi:zynqmp_ipi_send(RX) -> tessai_agent:tessai_rpmsg_cb_enter` | 1000 | 9.000 | 43.000 | 10.395 | 1.758 |
| 8 | `tessai_agent:tessai_rpmsg_cb_enter -> tessai_agent:tessai_rpmsg_cb_exit` | 1000 | 2.000 | 7.000 | 2.585 | 0.521 |
| 9 | `tessai_agent:tessai_rpmsg_cb_exit -> sdt_tessai_agent:wait_ioctl_exit` | 1000 | 3.000 | 267.000 | 3.676 | 8.457 |

## End-to-end total (microseconds, full span incl. excluded gaps)

| Count | Min | Max | Avg | Std |
|------:|----:|----:|----:|----:|
| 1000 | 120.000 | 402.000 | 131.759 | 9.244 |

## Histograms (microseconds)

### Step 0: `sdt_tessai_agent:cmd_submit_enter -> tessai_agent:tessai_cmd_submit_enter`

```
[    1.000,     2.700)    995 | ########################################
[    2.700,     4.400)      4 | 
[    4.400,     6.100)      0 | 
[    6.100,     7.800)      0 | 
[    7.800,     9.500)      0 | 
[    9.500,    11.200)      0 | 
[   11.200,    12.900)      0 | 
[   12.900,    14.600)      0 | 
[   14.600,    16.300)      0 | 
[   16.300,    18.000)      1 | 
```

### Step 1: `tessai_agent:tessai_cmd_submit_enter -> tessai_agent:tessai_rpmsg_send_enter`

```
[    0.000,     0.800)     19 | #
[    0.800,     1.600)    834 | ########################################
[    1.600,     2.400)    100 | #####
[    2.400,     3.200)      6 | 
[    3.200,     4.000)      0 | 
[    4.000,     4.800)      7 | 
[    4.800,     5.600)     23 | #
[    5.600,     6.400)      8 | 
[    6.400,     7.200)      1 | 
[    7.200,     8.000)      2 | 
```

### Step 2: `tessai_agent:tessai_rpmsg_send_enter -> zynqmp_ipi:zynqmp_ipi_send(TX)`

```
[    3.000,     4.400)    926 | ########################################
[    4.400,     5.800)     68 | ###
[    5.800,     7.200)      3 | 
[    7.200,     8.600)      2 | 
[    8.600,    10.000)      0 | 
[   10.000,    11.400)      0 | 
[   11.400,    12.800)      0 | 
[   12.800,    14.200)      0 | 
[   14.200,    15.600)      0 | 
[   15.600,    17.000)      1 | 
```

### Step 3: `zynqmp_ipi:zynqmp_ipi_send(TX) -> tessai_agent:tessai_rpmsg_send_exit`

```
[    0.000,     0.200)      4 | 
[    0.200,     0.400)      0 | 
[    0.400,     0.600)      0 | 
[    0.600,     0.800)      0 | 
[    0.800,     1.000)    186 | ##########
[    1.000,     1.200)    737 | ########################################
[    1.200,     1.400)      0 | 
[    1.400,     1.600)      0 | 
[    1.600,     1.800)      0 | 
[    1.800,     2.000)     73 | ####
```

### Step 4: `tessai_agent:tessai_rpmsg_send_exit -> zynqmp_ipi_gic:zynqmp_ipi_gic_entry`

```
[   89.000,    92.200)     13 | #
[   92.200,    95.400)      2 | 
[   95.400,    98.600)     32 | ##
[   98.600,   101.800)    196 | ##########
[  101.800,   105.000)    749 | ########################################
[  105.000,   108.200)      5 | 
[  108.200,   111.400)      2 | 
[  111.400,   114.600)      0 | 
[  114.600,   117.800)      0 | 
[  117.800,   121.000)      1 | 
```

### Step 5: `zynqmp_ipi_gic:zynqmp_ipi_gic_entry -> zynqmp_ipi:zynqmp_ipi_isr`

```
[    0.000,     0.400)    632 | ########################################
[    0.400,     0.800)      0 | 
[    0.800,     1.200)    367 | #######################
[    1.200,     1.600)      0 | 
[    1.600,     2.000)      0 | 
[    2.000,     2.400)      0 | 
[    2.400,     2.800)      0 | 
[    2.800,     3.200)      0 | 
[    3.200,     3.600)      0 | 
[    3.600,     4.000)      1 | 
```

### Step 6: `zynqmp_ipi:zynqmp_ipi_isr -> zynqmp_ipi:zynqmp_ipi_send(RX)`

```
[    4.000,     4.600)     20 | #
[    4.600,     5.200)    950 | ########################################
[    5.200,     5.800)      0 | 
[    5.800,     6.400)     29 | #
[    6.400,     7.000)      0 | 
[    7.000,     7.600)      0 | 
[    7.600,     8.200)      0 | 
[    8.200,     8.800)      0 | 
[    8.800,     9.400)      0 | 
[    9.400,    10.000)      1 | 
```

### Step 7: `zynqmp_ipi:zynqmp_ipi_send(RX) -> tessai_agent:tessai_rpmsg_cb_enter`

```
[    9.000,    12.400)    983 | ########################################
[   12.400,    15.800)      6 | 
[   15.800,    19.200)      5 | 
[   19.200,    22.600)      3 | 
[   22.600,    26.000)      1 | 
[   26.000,    29.400)      0 | 
[   29.400,    32.800)      0 | 
[   32.800,    36.200)      0 | 
[   36.200,    39.600)      0 | 
[   39.600,    43.000)      2 | 
```

### Step 8: `tessai_agent:tessai_rpmsg_cb_enter -> tessai_agent:tessai_rpmsg_cb_exit`

```
[    2.000,     2.500)    422 | ##############################
[    2.500,     3.000)      4 | 
[    3.000,     3.500)    571 | ########################################
[    3.500,     4.000)      0 | 
[    4.000,     4.500)      1 | 
[    4.500,     5.000)      0 | 
[    5.000,     5.500)      1 | 
[    5.500,     6.000)      0 | 
[    6.000,     6.500)      0 | 
[    6.500,     7.000)      1 | 
```

### Step 9: `tessai_agent:tessai_rpmsg_cb_exit -> sdt_tessai_agent:wait_ioctl_exit`

```
[    3.000,    29.400)    998 | ########################################
[   29.400,    55.800)      1 | 
[   55.800,    82.200)      0 | 
[   82.200,   108.600)      0 | 
[  108.600,   135.000)      0 | 
[  135.000,   161.400)      0 | 
[  161.400,   187.800)      0 | 
[  187.800,   214.200)      0 | 
[  214.200,   240.600)      0 | 
[  240.600,   267.000)      1 | 
```
