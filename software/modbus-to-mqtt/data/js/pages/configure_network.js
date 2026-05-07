import {reboot, wipeNetwork} from 'app';

window.initConfigureNetwork = async function initConfigureNetwork() {
    document.querySelector('#btn-reboot').onclick = reboot;
    const wipeBtn = document.querySelector('#btn-wipe-network');
    if (wipeBtn) wipeBtn.onclick = wipeNetwork;
}


