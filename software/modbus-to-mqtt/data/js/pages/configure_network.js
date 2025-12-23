import {API, reboot, rssiBadge, rssiToBars} from 'app';

window.initConfigureNetwork = async function initConfigureNetwork() {
    document.querySelector('#btn-reboot').onclick = reboot;
}


