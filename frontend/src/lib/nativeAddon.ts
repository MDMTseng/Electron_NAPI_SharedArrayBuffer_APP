import * as path from 'path';

function getAddonPath() {
    if(process.env.NODE_ENV === 'development') {
        console.log("DEV MODE");
        //get electron prj path
        return `${process.cwd()}/build/Release/addon`;
    } else {
        return `${__dirname}/../native/addon`;
    }
}

let addon: any;

try {
    addon = require(getAddonPath());
    // addon = window.get_nativeApi.get();
} catch (error) {
    console.error('Failed to load native addon:', error);
    // Provide mock implementations for development/testing
    addon = {
        setSharedBuffer: () => console.log('Mock: setSharedBuffer called'),
        setMessageCallback: () => console.log('Mock: setMessageCallback called'),
        startSendingData: () => console.log('Mock: startSendingData called'),
        stopSendingData: () => console.log('Mock: stopSendingData called'),
        triggerTestCallback: () => console.log('Mock: triggerTestCallback called'),
        cleanup: () => console.log('Mock: cleanup called'),
        loadPlugin: () => console.log('Mock: loadPlugin called'),
        unloadPlugin: () => console.log('Mock: unloadPlugin called'),
    };
}

export const nativeAddon = {
    setSharedBuffer: (
        buffer: ArrayBuffer,
        rendererToNativeSize: number,
        nativeToRendererSize: number
    ) => addon.setSharedBuffer(buffer, rendererToNativeSize, nativeToRendererSize),

    setMessageCallback: (callback: (buffer: ArrayBuffer) => void) => 
        addon.setMessageCallback(callback),

    startSendingData: (interval: number) => addon.startSendingData(interval),

    stopSendingData: () => addon.stopSendingData(),

    triggerTestCallback: () => addon.triggerTestCallback(),

    cleanup: () => addon.cleanup(),

    loadPlugin: (pluginPath: string) => addon.loadPlugin(pluginPath),

    unloadPlugin: () => addon.unloadPlugin(),
}; 