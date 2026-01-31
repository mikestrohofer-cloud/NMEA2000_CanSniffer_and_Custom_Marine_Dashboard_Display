#!/usr/bin/env python3
import os
import time
import subprocess

def restore_taskbar():
    print("Attempting to restore taskbar...")
    # Optional delay in case you need to allow transient issues to settle
    time.sleep(2)
    
    # Prepare a copy of the current environment and set X display variables.
    env = os.environ.copy()
    env["DISPLAY"] = ":0"
    env["XAUTHORITY"] = "/home/mikestrohofer/.Xauthority"
    
    # Log the environment settings for debugging
    print("Environment for taskbar restoration:")
    print("DISPLAY =", env.get("DISPLAY"))
    print("XAUTHORITY =", env.get("XAUTHORITY"))
    
    # Attempt to run the taskbar restoration command
    try:
        process = subprocess.Popen(
            ["/usr/bin/wf-panel-pi"],
            env=env
        )
        print("wf-panel-pi launched with PID:", process.pid)
    except Exception as e:
        print("Error launching wf-panel-pi:", e)

if __name__ == '__main__':
    restore_taskbar()

