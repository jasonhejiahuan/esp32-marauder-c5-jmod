// One visibility rule for device lists, metrics, charts and exports.
// Stable keys keep signal fluctuations from moving existing rows.
(function(root){
  class PresenceView {
    constructor(){this.reset();}
    reset(){this.order=new Map();this.next=0;}
    visible(items,hideOffline,key){
      for(const item of items)if(!this.order.has(key(item)))this.order.set(key(item),this.next++);
      const retained=new Set(items.map(key));
      for(const id of this.order.keys())if(!retained.has(id))this.order.delete(id);
      return items.filter(item=>!hideOffline||item.online!==false)
        .sort((a,b)=>this.order.get(key(a))-this.order.get(key(b)));
    }
  }
  if(typeof module!=='undefined')module.exports={PresenceView};else root.PresenceView=PresenceView;
})(typeof window!=='undefined'?window:globalThis);
