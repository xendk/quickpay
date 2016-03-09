<?php

/**
 * @file
 * Hooks provided by Drupal core and the System module.
 */

/**
 * @addtogroup hooks
 * @{
 */

/**
 * Get configured Quickpay object for an order.
 *
 * Technically not a hook, as it's called in the module that initiated a
 * payment. It should return an Quickpay object as configured by the module
 * for the given order. A simple module that only supports one QuickPay
 * configuration could just store settings in a variable and pass that to the
 * Quickpay constructor:
 * @code
 * return new Quickpay(variable_get('mymodule_quickpay_settings, array()));
 * @endcode
 *
 * @param string $order_number
 *   The order ID of the payment.
 *
 * @return Quickpay
 *   The configured quickpay instance.
 */
function hook_quickpay_factory($order_number) {
  // Load order.
  $order = commerce_order_load_by_number($order_number);
  // Get the payment_method from it.
  $payment_method = commerce_payment_method_instance_load($order->data['payment_method']);
  return new Quickpay($payment_method['settings']);
}

/**
 * Form payment callback from Quickpay.
 *
 * Allows the payment requesting module to react to transaction
 * authentication. Usually this involves moving the order from a "payment
 * pending" state to a "pending" (or "completed" if using autocapture) state.
 *
 * @param string $order_number
 *   The order number of the payment.
 * @param QuickpayTransaction $transaction
 *   The transaction object. Remember to call QuickpayTransaction::success()
 *   to check for success.
 */
function hook_quickpay_callback($order_number, $transaction) {
  // If the transaction has failed, we don't add any payment.
  if (!$transaction->success()) {
    return;
  }

  $payment_data = array(
    'type' => 'quickpay_txn_ready',
    'txn_id' => $transaction->store(),
    'txn' => $transaction,
  );
  $order = uc_order_load($order_number);
  $order->data['quickpay_settings'] = variable_get('uc_quickpay_settings');
  $order->data['quickpay_txn_id'] = $payment_data['txn_id'];
  uc_order_save($order);

  uc_payment_enter($order_number, 'quickpay', 0, 0, serialize($payment_data), theme('uc_quickpay_authorized_comment', array('txn' => $transaction)));
}

/**
 * @} End of "defgroup update_api".
 */
