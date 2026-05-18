# fast

## :brain: Why?

fast is a C command-line tool that runs an internet speed test using [Netflix fast.com](https://fast.com) written in less than 1k lines of code:

```bash
$ loc
--------------------------------------------------------------------------------
 Language             Files        Lines        Blank      Comment         Code
--------------------------------------------------------------------------------
 C                        1          847           82            0          765
 Markdown                 1           65           22            0           43
 Makefile                 1           30            6            0           24
--------------------------------------------------------------------------------
 Total                    3          942          110            0          832
--------------------------------------------------------------------------------
```

It measures your download speed by discovering test endpoints and fetching data from them.

The goal is to offer a simple, lightweight alternative for checking connection performance.

## :rocket: Get Started

### :zap: How to install

To install fast, you can use the following commands:

```bash
# Clone the repository
git clone https://github.com/AntonioBerna/fast.git

# Change directory
cd fast/

# Build and install
sudo make install
```

### :wastebasket: How to uninstall

To uninstall fast, you can use the following command:

```bash
# Uninstall
sudo make uninstall
```

### :rocket: Your first speed test

After the installation, you can run the following command:

```bash
fast --help
```

which will give us the following output:

```bash
$ fast --help
Usage
  fast
  fast > file

Choose at most one of --upload, --json, or --verbose.

Options
  --upload, -u   Measure upload speed in addition to download speed
  --json         JSON output
  --verbose      Include latency and server location information
  --help, -h     Show this help
```
