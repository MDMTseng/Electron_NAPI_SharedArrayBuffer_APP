export class Throttle {
    private delay: number;
    private timer: NodeJS.Timeout | null;
    private lastRun: number;
    private queued: boolean;

    constructor(delay = 100) {
        this.delay = delay;
        this.timer = null;
        this.lastRun = 0;
        this.queued = false;
    }

    /**
     * Schedule a function to be executed
     * @param fn The function to throttle
     * @param immediate Whether to run immediately if possible
     * @returns A promise that resolves when the function executes
     */
    schedule(fn: () => void, immediate = false): Promise<void> {
        return new Promise((resolve) => {
            const now = Date.now();
            const timeSinceLastRun = now - this.lastRun;

            // Clear any existing timer
            if (this.timer) {
                clearTimeout(this.timer);
                this.timer = null;
            }

            // If enough time has passed and immediate execution is requested, run now
            if (timeSinceLastRun >= this.delay && immediate) {
                this.lastRun = now;
                fn();
                resolve();
                return;
            }

            // Otherwise, schedule for later
            this.timer = setTimeout(() => {
                this.lastRun = Date.now();
                fn();
                this.timer = null;
                resolve();
            }, Math.max(0, this.delay - timeSinceLastRun));
        });
    }

    /**
     * Cancel any pending execution
     */
    cancel(): void {
        if (this.timer) {
            clearTimeout(this.timer);
            this.timer = null;
        }
    }

    /**
     * Change the delay time
     * @param newDelay New delay in milliseconds
     */
    setDelay(newDelay: number): void {
        this.delay = newDelay;
    }

    /**
     * Check if there's a pending execution
     */
    isPending(): boolean {
        return this.timer !== null;
    }

    /**
     * Get time until next possible execution
     * @returns Milliseconds until next possible execution
     */
    getTimeUntilNextRun(): number {
        const timeSinceLastRun = Date.now() - this.lastRun;
        return Math.max(0, this.delay - timeSinceLastRun);
    }
} 